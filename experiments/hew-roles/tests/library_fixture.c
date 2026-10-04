#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct { uint8_t *ptr; uint32_t offset, len; } Bytes;
extern uint8_t *hew_bytes_new(uint32_t);

Bytes mc_test_pair(void) {
    int fd[2], capacity = 4096;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, fd)) return (Bytes){0};
    if (setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof capacity)) {
        close(fd[0]); close(fd[1]); return (Bytes){0};
    }
    Bytes result = {hew_bytes_new(8), 0, 8};
    uint32_t descriptors[2] = {(uint32_t)fd[0], (uint32_t)fd[1]};
    memcpy(result.ptr, descriptors, 8);
    return result;
}
