#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <openssl/evp.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct { uint8_t *ptr; uint32_t offset, len; } Bytes;
extern uint8_t *hew_bytes_new(uint32_t);
extern char **environ;
static volatile sig_atomic_t running = 1;
int32_t mc_supervision_testing(void) {
#ifdef MC_SUPERVISION_TEST
    return 1;
#else
    return 0;
#endif
}
static void stopped(int sig) { (void)sig; running = 0; }
int64_t mc_now(void) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
int64_t mc_monotonic(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
void mc_signals(void) { signal(SIGTERM, stopped); signal(SIGINT, stopped); }
int32_t mc_running(void) { return running; }
static int string(const Bytes *b, char *out, size_t cap) {
    if (!b->len || b->len >= cap || memchr(b->ptr + b->offset, 0, b->len)) return 0;
    memcpy(out, b->ptr + b->offset, b->len); out[b->len] = 0; return 1;
}
int32_t mc_directory(const Bytes *path) {
    char name[4096]; struct stat st;
    if (!string(path, name, sizeof name)) return 0;
    if (mkdir(name, 0700) && errno != EEXIST) return 0;
    return !lstat(name, &st) && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && (st.st_mode & 0777) == 0700;
}
int32_t mc_wait(int32_t fd, int32_t writable, int32_t timeout) {
    if (fd < 0 || timeout < 0 || timeout > 30000) return -EINVAL;
    struct pollfd p = {fd, writable ? POLLOUT : POLLIN, 0};
    int result = poll(&p, 1, timeout);
    if (result < 0) return -errno;
    return result;
}
int32_t mc_errno_kind(int32_t code) {
    if (code == EINTR) return 1;
    if (code == EAGAIN || code == EWOULDBLOCK) return 2;
    return 0;
}
Bytes mc_read_once(int32_t fd) {
    uint8_t data[4097] = {0};
    ssize_t count = recv(fd, data + 1, sizeof data - 1, MSG_DONTWAIT);
    size_t size = 1;
    if (count > 0) { data[0] = 1; size += (size_t)count; }
    else if (count == 0) data[0] = 2;
    else if (!mc_errno_kind(errno)) {
        uint32_t error = (uint32_t)errno;
        data[0] = 3; size = 5;
        for (unsigned i = 0; i < 4; i++) data[i+1] = (uint8_t)(error >> (8*i));
    }
    Bytes out = {hew_bytes_new((uint32_t)size), 0, (uint32_t)size};
    memcpy(out.ptr, data, size); return out;
}
int32_t mc_write_once(int32_t fd, const Bytes *data) {
    if (data->len > 65536) return -EINVAL;
    ssize_t size = send(fd, data->ptr + data->offset, data->len, MSG_NOSIGNAL | MSG_DONTWAIT);
    return size < 0 ? -errno : (int32_t)size;
}
void mc_close(int32_t fd) {
    if (fd >= 0) {
        /* A readiness registration may pin a duplicate; wake it on owner loss. */
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}
static int hash_file(const char *path, uint8_t out[32]) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    uint8_t chunk[16384]; size_t size; unsigned written = 0;
    int ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1;
    while (ok && (size = fread(chunk, 1, sizeof chunk, file)) > 0)
        ok = EVP_DigestUpdate(ctx, chunk, size) == 1;
    ok = ok && !ferror(file) && EVP_DigestFinal_ex(ctx, out, &written) == 1 && written == 32;
    EVP_MD_CTX_free(ctx); fclose(file);
    return ok;
}
static int hash_matches(const uint8_t hash[32], const char *hex) {
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++)
        if (hex[2*i] != digits[hash[i] >> 4] || hex[2*i+1] != digits[hash[i] & 15]) return 0;
    return 1;
}
int32_t mc_verify_worker(const Bytes *path, const Bytes *expected) {
    char name[4096]; struct stat st; uint8_t hash[32]; unsigned size = 0;
    if (!string(path, name, sizeof name) || expected->len != 64) return 0;
    int fd = open(name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 1 || st.st_size > 4*1024*1024) {
        close(fd); return 0;
    }
    char *data = malloc((size_t)st.st_size + 1);
    if (!data) { close(fd); return 0; }
    size_t at = 0;
    while (at < (size_t)st.st_size) {
        ssize_t n = read(fd, data + at, (size_t)st.st_size - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        at += (size_t)n;
    }
    close(fd); data[at] = 0;
    int ok = at == (size_t)st.st_size && !memchr(data, 0, at) &&
        EVP_Digest(data, at, hash, &size, EVP_sha256(), NULL) == 1 && size == 32 &&
        hash_matches(hash, (const char *)expected->ptr + expected->offset);
    char *entry = data;
    while (ok && *entry) {
        char *end = strchr(entry, '\n');
        if (!end || end - entry < 67 || entry[64] != '\t' || entry[65] != '/') { ok = 0; break; }
        *end = 0;
        ok = hash_file(entry + 65, hash) && hash_matches(hash, entry);
        if (!ok) fprintf(stderr, "Native worker input changed or unavailable: %s\n", entry + 65);
        entry = end + 1;
    }
    free(data); return ok;
}
Bytes mc_signal(int32_t snr, int32_t rssi) {
    float values[2] = {(float)rssi, (float)snr / 4.0f};
    Bytes out = {hew_bytes_new(9), 0, 9};
    memcpy(out.ptr, values, 8);
    out.ptr[8] = rssi == 127 && snr == -128 ? 1 : 0;
    return out;
}
int32_t mc_snr_quarter(const Bytes *metadata) {
    if (metadata->len != 9 || metadata->ptr[metadata->offset + 8] != 0) return 0;
    float value;
    memcpy(&value, metadata->ptr + metadata->offset + 4, sizeof(value));
    if (!(value >= -32.0f && value <= 32.0f)) return 0;
    return (int32_t)(value * 4.0f);
}
Bytes mc_spawn_duplex(const Bytes *executable, const Bytes *arguments) {
    char program[4096], data[16384], *argv[34];
    if (!string(executable, program, sizeof program) || program[0] != '/' ||
        arguments->len > sizeof data) return (Bytes){0};
    if (arguments->len) memcpy(data, arguments->ptr + arguments->offset, arguments->len);
    unsigned count = 1;
    size_t at = 0;
    argv[0] = program;
    while (at < arguments->len) {
        char *end = memchr(data + at, 0, arguments->len - at);
        if (!end || count >= 33) return (Bytes){0};
        argv[count++] = data + at;
        at = (size_t)(end - data) + 1;
    }
    argv[count] = NULL;
    int fd[2]; pid_t pid;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fd)) return (Bytes){0};
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) { close(fd[0]); close(fd[1]); return (Bytes){0}; }
    int failed = posix_spawn_file_actions_adddup2(&actions, fd[1], STDIN_FILENO);
    if (!failed) failed = posix_spawn_file_actions_adddup2(&actions, fd[1], STDOUT_FILENO);
    if (!failed && fd[0] != STDIN_FILENO && fd[0] != STDOUT_FILENO) failed = posix_spawn_file_actions_addclose(&actions, fd[0]);
    if (!failed && fd[1] != STDIN_FILENO && fd[1] != STDOUT_FILENO) failed = posix_spawn_file_actions_addclose(&actions, fd[1]);
    if (!failed) failed = posix_spawn(&pid, program, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions); close(fd[1]);
    if (failed) { close(fd[0]); return (Bytes){0}; }
    Bytes out = {hew_bytes_new(8), 0, 8};
    uint32_t values[2] = {(uint32_t)fd[0], (uint32_t)pid};
    for (unsigned word = 0; word < 2; word++)
        for (unsigned byte = 0; byte < 4; byte++) out.ptr[word*4+byte] = (uint8_t)(values[word] >> (8*byte));
    return out;
}
int32_t mc_child_poll(int32_t pid) {
    if (pid <= 0) return -EINVAL;
    int status;
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result < 0) return -errno;
    if (!result) return 0;
    return WIFEXITED(status) ? WEXITSTATUS(status) + 1 : WTERMSIG(status) + 257;
}
int32_t mc_child_signal(int32_t pid, int32_t force) {
    if (pid <= 0 || (force != 0 && force != 1)) return -EINVAL;
    return kill(pid, force ? SIGKILL : SIGTERM) ? -errno : 0;
}
int32_t mc_child_no_entry(int32_t code) { return code == ECHILD; }
void mc_shutdown(int32_t fd) { if (fd >= 0) shutdown(fd, SHUT_RDWR); }
/* Normal timed retirement is in Hew. This native finalizer only force-reaps a
   still-owned child, like std.process.Child's last-resort cleanup. */
void mc_child_drop(int32_t pid) {
    if (pid <= 0) return;
    pid_t result;
    do { result = waitpid(pid, NULL, WNOHANG); } while (result < 0 && errno == EINTR);
    if (result != 0) return;
    kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}
int32_t mc_dial_start(const Bytes *host, int32_t port) {
    char address[64]; struct sockaddr_in addr = {0};
    if (!string(host, address, sizeof address) || port < 1 || port > 65535) return -EINVAL;
    addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, address, &addr.sin_addr) != 1) return -EINVAL;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -errno;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0 && errno != EINPROGRESS) {
        int error = errno; close(fd); return -error;
    }
    return fd;
}

int32_t mc_socket_error(int32_t fd) {
    int error = 0; socklen_t size = sizeof error;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size)) return errno;
    return error;
}
