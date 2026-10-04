#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <malloc.h>
#include <dirent.h>
#include <netdb.h>
#include <stdio.h>

typedef struct { uint8_t *ptr; uint32_t offset, len; } Bytes;
extern uint8_t *hew_bytes_new(uint32_t);
typedef struct { int fd; int64_t token; } Socket;
typedef struct { int fd; int64_t next; Socket sockets[256]; } Listener;
static volatile sig_atomic_t interrupted;
static void interrupt(int signal) { (void)signal; interrupted = 1; }
static Bytes reply(uint8_t status, const uint8_t *data, uint32_t size) {
    Bytes out = {hew_bytes_new(size + 1), 0, size + 1};
    out.ptr[0] = status;
    if (size) memcpy(out.ptr + 1, data, size);
    return out;
}
int64_t hew_listener_now(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value)) return -1;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}
int32_t hew_listener_running(void) { return !interrupted; }
int64_t hew_listener_wall(void) { return (int64_t)time(NULL); }
int64_t hew_listener_heap(void) {
    struct mallinfo2 memory = mallinfo2();
    return (int64_t)(memory.uordblks + memory.hblkhd);
}
int64_t hew_listener_threads(void) {
    DIR *tasks = opendir("/proc/self/task");
    if (!tasks) return -errno;
    int64_t count = 0; struct dirent *entry;
    while ((entry = readdir(tasks))) if (entry->d_name[0] != '.') count++;
    closedir(tasks);
    return count;
}
int32_t hew_listener_loopback(const Bytes *host) {
    char text[INET6_ADDRSTRLEN];
    if (!host->len || host->len >= sizeof text || memchr(host->ptr + host->offset, 0, host->len)) return 0;
    memcpy(text, host->ptr + host->offset, host->len); text[host->len] = 0;
    struct in_addr v4; struct in6_addr v6;
    if (inet_pton(AF_INET, text, &v4) == 1) return (ntohl(v4.s_addr) >> 24) == 127;
    return inet_pton(AF_INET6, text, &v6) == 1 && IN6_IS_ADDR_LOOPBACK(&v6);
}
int32_t hew_listener_equal(const Bytes *a, const Bytes *b) {
    uint8_t left[SHA256_DIGEST_LENGTH], right[SHA256_DIGEST_LENGTH];
    if (!SHA256(a->len ? a->ptr + a->offset : (const uint8_t *)"", a->len, left) ||
        !SHA256(b->len ? b->ptr + b->offset : (const uint8_t *)"", b->len, right)) return 0;
    int result = CRYPTO_memcmp(left, right, sizeof left) == 0;
    OPENSSL_cleanse(left, sizeof left); OPENSSL_cleanse(right, sizeof right);
    return result;
}
Bytes hew_listener_config(const Bytes *path) {
    if (!path->len || path->len > 4096 || memchr(path->ptr + path->offset, 0, path->len))
        return reply(2, NULL, 0);
    char *name = malloc(path->len + 1);
    if (!name) return reply(2, NULL, 0);
    memcpy(name, path->ptr + path->offset, path->len); name[path->len] = 0;
    int fd = open(name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); free(name);
    struct stat stat;
    if (fd < 0) return reply(3, NULL, 0);
    if (fstat(fd, &stat)) { close(fd); return reply(4, NULL, 0); }
    if (!S_ISREG(stat.st_mode) || stat.st_uid != geteuid() ||
        (stat.st_mode & 077) || stat.st_size <= 0 || stat.st_size > 4096) {
        close(fd); return reply(5, NULL, 0);
    }
    uint8_t buffer[4097]; size_t size = 0;
    while (size < sizeof buffer) {
        ssize_t n = read(fd, buffer + size, sizeof buffer - size);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { close(fd); return reply(6, NULL, 0); }
        if (!n) break;
        size += (size_t)n;
    }
    close(fd);
    if (!size || size > 4096) return reply(2, NULL, 0);
    Bytes out = reply(1, buffer, (uint32_t)size);
    OPENSSL_cleanse(buffer, sizeof buffer);
    return out;
}
void hew_listener_close(int64_t handle) {
    Listener *owner = (Listener *)(uintptr_t)handle;
    if (!owner) return;
    for (size_t i = 0; i < 256; i++) if (owner->sockets[i].fd >= 0) close(owner->sockets[i].fd);
    if (owner->fd >= 0) close(owner->fd);
    free(owner);
}
int64_t hew_listener_open(const Bytes *host, int64_t port) {
    if (!host->len || host->len > 253 || port < 0 || port > 65535 ||
        memchr(host->ptr + host->offset, 0, host->len)) return -EINVAL;
    char text[254];
    memcpy(text, host->ptr + host->offset, host->len); text[host->len] = 0;
    struct sockaddr_storage address = {0};
    struct sockaddr_in *v4 = (void *)&address;
    struct sockaddr_in6 *v6 = (void *)&address;
    socklen_t length;
    if (inet_pton(AF_INET, text, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET; v4->sin_port = htons((uint16_t)port); length = sizeof *v4;
    } else if (inet_pton(AF_INET6, text, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6; v6->sin6_port = htons((uint16_t)port); length = sizeof *v6;
    } else {
        struct addrinfo hints = {0}, *addresses = NULL, *selected = NULL;
        char service[6]; snprintf(service, sizeof service, "%lld", (long long)port);
        hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
        int error = getaddrinfo(text, service, &hints, &addresses);
        if (error) return error == EAI_AGAIN ? -EAGAIN : -EINVAL;
        for (struct addrinfo *candidate = addresses; candidate; candidate = candidate->ai_next)
            if (candidate->ai_family == AF_INET) { selected = candidate; break; }
        if (!selected)
            for (struct addrinfo *candidate = addresses; candidate; candidate = candidate->ai_next)
                if (candidate->ai_family == AF_INET6) { selected = candidate; break; }
        if (!selected || selected->ai_addrlen > sizeof address) {
            freeaddrinfo(addresses); return -EINVAL;
        }
        length = selected->ai_addrlen;
        memcpy(&address, selected->ai_addr, length);
        freeaddrinfo(addresses);
    }
    Listener *owner = calloc(1, sizeof *owner);
    if (!owner) return -ENOMEM;
    owner->fd = -1; owner->next = 1;
    for (size_t i = 0; i < 256; i++) owner->sockets[i].fd = -1;
    owner->fd = socket(address.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int reuse = 1;
    if (owner->fd >= 0 && address.ss_family == AF_INET6 && IN6_IS_ADDR_UNSPECIFIED(&v6->sin6_addr)) {
        int only_v6 = 0;
        if (setsockopt(owner->fd, IPPROTO_IPV6, IPV6_V6ONLY, &only_v6, sizeof only_v6)) {
            int error = errno; hew_listener_close((int64_t)(uintptr_t)owner); return -error;
        }
    }
    if (owner->fd < 0 || setsockopt(owner->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) ||
        bind(owner->fd, (void *)&address, length) || listen(owner->fd, 128)) {
        int error = errno; hew_listener_close((int64_t)(uintptr_t)owner); return -error;
    }
    interrupted = 0;
    struct sigaction action = {0};
    action.sa_handler = interrupt; sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) {
        int error = errno; hew_listener_close((int64_t)(uintptr_t)owner); return -error;
    }
    return (int64_t)(uintptr_t)owner;
}
int64_t hew_listener_port(int64_t handle) {
    Listener *owner = (Listener *)(uintptr_t)handle;
    struct sockaddr_storage address; socklen_t length = sizeof address;
    if (getsockname(owner->fd, (void *)&address, &length)) return -errno;
    return address.ss_family == AF_INET ? ntohs(((struct sockaddr_in *)&address)->sin_port)
        : ntohs(((struct sockaddr_in6 *)&address)->sin6_port);
}
int64_t hew_listener_accept(int64_t handle) {
    Listener *owner = (Listener *)(uintptr_t)handle;
    int fd = accept(owner->fd, NULL, NULL);
    if (fd < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -errno;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) || fcntl(fd, F_SETFD, FD_CLOEXEC)) {
        int error = errno; close(fd); return -error;
    }
    int send_buffer = 65536;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof send_buffer)) {
        int error = errno; close(fd); return -error;
    }
    for (size_t i = 0; i < 256; i++) if (owner->sockets[i].fd < 0) {
        owner->sockets[i] = (Socket){ fd, owner->next++ };
        return owner->sockets[i].token;
    }
    close(fd); return -EMFILE;
}
static Socket *lookup(Listener *owner, int64_t token) {
    for (size_t i = 0; i < 256; i++)
        if (owner->sockets[i].fd >= 0 && owner->sockets[i].token == token) return &owner->sockets[i];
    return NULL;
}
int32_t hew_listener_descriptor(int64_t handle, int64_t token) {
    Listener *owner = (Listener *)(uintptr_t)handle;
    if (!owner) return -EINVAL;
    if (!token) return owner->fd;
    Socket *socket = lookup(owner, token);
    return socket ? socket->fd : -ENOENT;
}
void hew_listener_drop(int64_t handle, int64_t token) {
    Socket *socket = lookup((Listener *)(uintptr_t)handle, token);
    if (socket) { close(socket->fd); socket->fd = -1; }
}
Bytes hew_listener_read(int64_t handle, int64_t token) {
    Socket *socket = lookup((Listener *)(uintptr_t)handle, token);
    if (!socket) return reply(3, NULL, 0);
    uint8_t buffer[4096];
    ssize_t size = recv(socket->fd, buffer, sizeof buffer, 0);
    if (size > 0) return reply(1, buffer, (uint32_t)size);
    if (!size) return reply(2, NULL, 0);
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return reply(0, NULL, 0);
    return reply(3, NULL, 0);
}
int64_t hew_listener_write(int64_t handle, int64_t token, const Bytes *data) {
    Socket *socket = lookup((Listener *)(uintptr_t)handle, token);
    if (!socket || data->len > 65536) return -EINVAL;
    ssize_t size = send(socket->fd, data->ptr + data->offset, data->len, MSG_NOSIGNAL);
    if (size >= 0) return size;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -errno;
}
