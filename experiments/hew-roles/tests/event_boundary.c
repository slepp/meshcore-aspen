#define _GNU_SOURCE
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include "../event_bridge.c"

uint8_t *hew_bytes_new(uint32_t size) {
    uint8_t *data = malloc(size);
    assert(data);
    return data;
}

static uint64_t number(const uint8_t *data, unsigned length) {
    uint64_t value = 0;
    for (unsigned i = 0; i < length; i++) value |= (uint64_t)data[i] << (8 * i);
    return value;
}

static void expect_ready(int64_t group, int64_t key, unsigned flags) {
    Bytes value = mc_events_wait(group);
    assert(value.len == 13 && value.ptr[0] == 1);
    assert(number(value.ptr + 1, 8) == (uint64_t)key);
    assert((number(value.ptr + 9, 4) & flags) == flags);
    free(value.ptr);
}

typedef struct { int64_t group; int notify; Bytes result; } Waiter;

static void *waiter(void *argument) {
    Waiter *wait = argument;
    wait->result = mc_events_wait(wait->group);
    assert(write(wait->notify, "x", 1) == 1);
    mc_events_release(wait->group);
    return NULL;
}

static void stopped_wait(void) {
    int64_t group = mc_events_new();
    assert(group > 0);
    int notification[2];
    assert(pipe2(notification, O_CLOEXEC) == 0);
    Waiter waiting = {.group = group, .notify = notification[1]};
    pthread_t thread;
    mc_events_retain(group);
    assert(pthread_create(&thread, NULL, waiter, &waiting) == 0);
    assert(mc_events_stop(group) == 0);
    struct pollfd ready = {.fd = notification[0], .events = POLLIN};
    assert(poll(&ready, 1, 2000) == 1);
    assert(pthread_join(thread, NULL) == 0);
    assert(waiting.result.len == 1 && waiting.result.ptr[0] == 2);
    free(waiting.result.ptr);
    assert(mc_events_add(group, notification[0], 1) == -ECANCELED);
    mc_events_release(group);
    close(notification[0]); close(notification[1]);
}

int main(void) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    int64_t group = mc_events_new();
    assert(group > 0);
    assert(mc_events_add(group, -1, 1) == -EINVAL);
    assert(mc_events_add(group, sockets[0], 0) == -EINVAL);
    int64_t first = mc_events_add(group, sockets[0], 1);
    assert(first > 0);
    assert(write(sockets[1], "hello", 5) == 5);
    expect_ready(group, first, 1);
    char data[5];
    assert(read(sockets[0], data, sizeof data) == sizeof data);
    assert(memcmp(data, "hello", 5) == 0);
    assert(mc_events_arm(group, first, 1) == 0);
    assert(shutdown(sockets[1], SHUT_WR) == 0);
    expect_ready(group, first, 4);
    assert(mc_events_remove(group, first) == 0);
    assert(mc_events_arm(group, first, 1) == -ENOENT);
    assert(mc_events_remove(group, first) == -ENOENT);
    close(sockets[0]); close(sockets[1]);

    // A registration pins its socket even if its original FD number is reused.
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    int original = sockets[0];
    int64_t old = mc_events_add(group, original, 1);
    close(original);
    int replacement[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, replacement) == 0);
    assert(replacement[0] == original);
    assert(write(replacement[1], "new", 3) == 3);
    assert(write(sockets[1], "old", 3) == 3);
    expect_ready(group, old, 1);
    assert(mc_events_remove(group, old) == 0);
    int64_t next = mc_events_add(group, replacement[0], 1);
    assert(next > old && old > first);
    expect_ready(group, next, 1);
    assert(mc_events_remove(group, next) == 0);
    close(sockets[1]); close(replacement[0]); close(replacement[1]);
    assert(mc_events_wake(group) == 0);
    assert(mc_events_wake(group) == 0);
    expect_ready(group, 0, 0);
    mc_events_release(group);
    int64_t parent = mc_events_new(), child = mc_events_new();
    assert(parent > 0 && child > 0);
    int64_t channel = mc_events_add(parent, mc_events_descriptor(child), 1);
    assert(channel > 0);
    Bytes idle = mc_events_drain(parent);
    assert(idle.len == 1 && idle.ptr[0] == 1);
    free(idle.ptr);
    assert(mc_events_wake(child) == 0);
    expect_ready(parent, channel, 1);
    expect_ready(child, 0, 0);
    assert(mc_events_arm(parent, channel, 1) == 0);
    idle = mc_events_drain(parent);
    assert(idle.len == 1 && idle.ptr[0] == 1);
    free(idle.ptr);
    assert(mc_events_wake(child) == 0);
    expect_ready(parent, channel, 1);
    expect_ready(child, 0, 0);
    assert(mc_events_remove(parent, channel) == 0);
    mc_events_release(child); mc_events_release(parent);
    for (int i = 0; i < 100; i++) stopped_wait();
    puts("EVENT_NATIVE_OK");
}
