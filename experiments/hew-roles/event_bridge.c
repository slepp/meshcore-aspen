#define _GNU_SOURCE
#include "event_bridge.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

typedef struct { uint8_t *ptr; uint32_t offset, len; } Bytes;
extern uint8_t *hew_bytes_new(uint32_t);
typedef struct { int fd; uint64_t key; } Registration;
typedef struct {
    atomic_uint references;
    atomic_int stopped;
    atomic_flag waiting;
    pthread_mutex_t lock;
    int poller, wake;
    uint64_t next;
    Registration registrations[1024];
} Events;

int64_t mc_events_now(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -errno;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int64_t mc_events_new(void) {
    Events *events = calloc(1, sizeof *events);
    if (!events) return -ENOMEM;
    events->poller = events->wake = -1;
    int error = pthread_mutex_init(&events->lock, NULL);
    if (error) { free(events); return -error; }
    for (size_t i = 0; i < 1024; i++) events->registrations[i].fd = -1;
    atomic_init(&events->references, 1);
    atomic_init(&events->stopped, 0);
    atomic_flag_clear(&events->waiting);
    events->next = 1;
    events->poller = epoll_create1(EPOLL_CLOEXEC);
    if (events->poller >= 0) events->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    struct epoll_event wake = {.events = EPOLLIN, .data.u64 = 0};
    if (events->poller < 0 || events->wake < 0 ||
        epoll_ctl(events->poller, EPOLL_CTL_ADD, events->wake, &wake)) {
        error = errno;
        if (events->wake >= 0) close(events->wake);
        if (events->poller >= 0) close(events->poller);
        pthread_mutex_destroy(&events->lock); free(events); return -error;
    }
    return (int64_t)(uintptr_t)events;
}

void mc_events_retain(int64_t handle) {
    Events *events = (Events *)(uintptr_t)handle;
    atomic_fetch_add(&events->references, 1);
}

int32_t mc_events_wake(int64_t handle) {
    Events *events = (Events *)(uintptr_t)handle;
    uint64_t value = 1;
    ssize_t result;
    do { result = write(events->wake, &value, sizeof value); } while (result < 0 && errno == EINTR);
    return result == sizeof value || (result < 0 && errno == EAGAIN) ? 0 : -errno;
}

int32_t mc_events_stop(int64_t handle) {
    Events *events = (Events *)(uintptr_t)handle;
    atomic_store(&events->stopped, 1);
    return mc_events_wake(handle);
}

void mc_events_release(int64_t handle) {
    Events *events = (Events *)(uintptr_t)handle;
    if (atomic_fetch_sub(&events->references, 1) != 1) return;
    for (size_t i = 0; i < 1024; i++)
        if (events->registrations[i].fd >= 0) close(events->registrations[i].fd);
    close(events->wake); close(events->poller);
    pthread_mutex_destroy(&events->lock); free(events);
}

static uint32_t interest(int32_t flags) {
    return EPOLLONESHOT | EPOLLRDHUP | ((flags & 1) ? EPOLLIN : 0) |
        ((flags & 2) ? EPOLLOUT : 0);
}

int64_t mc_events_add(int64_t handle, int32_t fd, int32_t flags) {
    Events *events = (Events *)(uintptr_t)handle;
    if (fd < 0 || flags < 1 || flags > 3) return -EINVAL;
    pthread_mutex_lock(&events->lock);
    int64_t result = -EMFILE;
    if (atomic_load(&events->stopped)) result = -ECANCELED;
    else if (events->next > INT64_MAX) result = -EOVERFLOW;
    else for (size_t i = 0; i < 1024; i++) if (events->registrations[i].fd < 0) {
        int owned = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (owned < 0) { result = -errno; break; }
        uint64_t key = events->next++;
        struct epoll_event event = {.events = interest(flags), .data.u64 = key};
        if (epoll_ctl(events->poller, EPOLL_CTL_ADD, owned, &event)) {
            result = -errno; close(owned); break;
        }
        events->registrations[i] = (Registration){owned, key};
        result = (int64_t)key; break;
    }
    pthread_mutex_unlock(&events->lock);
    return result;
}

int32_t mc_events_arm(int64_t handle, int64_t key, int32_t flags) {
    Events *events = (Events *)(uintptr_t)handle;
    if (key <= 0 || flags < 1 || flags > 3) return -EINVAL;
    pthread_mutex_lock(&events->lock);
    int result = -ENOENT;
    if (atomic_load(&events->stopped)) result = -ECANCELED;
    else for (size_t i = 0; i < 1024; i++) if (events->registrations[i].key == (uint64_t)key &&
        events->registrations[i].fd >= 0) {
        struct epoll_event event = {.events = interest(flags), .data.u64 = (uint64_t)key};
        result = epoll_ctl(events->poller, EPOLL_CTL_MOD, events->registrations[i].fd, &event)
            ? -errno : 0;
        break;
    }
    pthread_mutex_unlock(&events->lock);
    return result;
}

int32_t mc_events_remove(int64_t handle, int64_t key) {
    Events *events = (Events *)(uintptr_t)handle;
    if (key <= 0) return -EINVAL;
    pthread_mutex_lock(&events->lock);
    int result = -ENOENT;
    for (size_t i = 0; i < 1024; i++) if (events->registrations[i].key == (uint64_t)key &&
        events->registrations[i].fd >= 0) {
        int fd = events->registrations[i].fd;
        result = epoll_ctl(events->poller, EPOLL_CTL_DEL, fd, NULL) ? -errno : 0;
        if (!result) {
            close(fd);
            events->registrations[i] = (Registration){-1, 0};
        }
        break;
    }
    pthread_mutex_unlock(&events->lock);
    return result;
}

static void integer(uint8_t *out, uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; i++) out[i] = (uint8_t)(value >> (8 * i));
}

int32_t mc_events_descriptor(int64_t handle) {
    return ((Events *)(uintptr_t)handle)->poller;
}

static Bytes read_events(int64_t handle, int timeout) {
    Events *events = (Events *)(uintptr_t)handle;
    uint8_t data[1 + 64 * 12] = {0};
    uint32_t length = 1;
    if (atomic_flag_test_and_set(&events->waiting)) {
        data[0] = 3; length = 5; integer(data + 1, EBUSY, 4);
    } else {
        struct epoll_event ready[64];
        int count = 0;
        if (!atomic_load(&events->stopped)) {
            do { count = epoll_wait(events->poller, ready, 64, timeout); }
            while (count < 0 && errno == EINTR && !atomic_load(&events->stopped));
        }
        if (atomic_load(&events->stopped)) data[0] = 2;
        else if (count < 0) {
            data[0] = 3; length = 5; integer(data + 1, (uint32_t)errno, 4);
        } else {
            data[0] = 1;
            for (int i = 0; i < count; i++) {
                if (!ready[i].data.u64) {
                    uint64_t value;
                    ssize_t n;
                    do { n = read(events->wake, &value, sizeof value); } while (n < 0 && errno == EINTR);
                    if (n < 0 && errno != EAGAIN) {
                        data[0] = 3; length = 5; integer(data + 1, (uint32_t)errno, 4); break;
                    }
                }
                integer(data + length, ready[i].data.u64, 8);
                uint32_t flags = ((ready[i].events & EPOLLIN) ? 1 : 0) |
                    ((ready[i].events & EPOLLOUT) ? 2 : 0) |
                    ((ready[i].events & (EPOLLHUP | EPOLLRDHUP)) ? 4 : 0) |
                    ((ready[i].events & EPOLLERR) ? 8 : 0);
                integer(data + length + 8, flags, 4); length += 12;
            }
        }
        atomic_flag_clear(&events->waiting);
    }
    Bytes out = {hew_bytes_new(length), 0, length};
    memcpy(out.ptr, data, length); return out;
}

Bytes mc_events_wait(int64_t handle) {
    return read_events(handle, -1);
}

Bytes mc_events_drain(int64_t handle) {
    return read_events(handle, 0);
}
