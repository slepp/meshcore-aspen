#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

typedef struct { uint8_t *ptr; uint32_t offset, len; } Bytes;
extern uint8_t *hew_bytes_new(uint32_t);

static int path_string(const Bytes *input, char *path, size_t capacity) {
    if (!input->len || input->len >= capacity || memchr(input->ptr + input->offset, 0, input->len)) return 0;
    memcpy(path, input->ptr + input->offset, input->len); path[input->len] = 0;
    return 1;
}

int32_t mc_owner_listen(const Bytes *input) {
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    struct stat st;
    if (!path_string(input, address.sun_path, sizeof(address.sun_path))) return -ENAMETOOLONG;
    char parent[sizeof(address.sun_path)];
    strcpy(parent, address.sun_path);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return -EINVAL;
    *slash = 0;
    if (lstat(parent, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0777) != 0700) return -EACCES;
    if (!lstat(address.sun_path, &st)) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0777) != 0600) return -EACCES;
        int probe = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (probe < 0) return -errno;
        int result = connect(probe, (struct sockaddr *)&address, sizeof(address));
        int error = errno; close(probe);
        if (result == 0 || error != ECONNREFUSED) return -EADDRINUSE;
        if (unlink(address.sun_path)) return -errno;
    } else if (errno != ENOENT) return -errno;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -errno;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address))) {
        int error = errno; close(fd); return -error;
    }
    if (chmod(address.sun_path, 0600) || listen(fd, 8)) {
        int error = errno; close(fd); unlink(address.sun_path); return -error;
    }
    return fd;
}

int64_t mc_owner_inode(const Bytes *input) {
    char path[108]; struct stat st;
    return path_string(input, path, sizeof(path)) && !lstat(path, &st) ? (int64_t)st.st_ino : -1;
}

void mc_owner_close(int32_t fd, const Bytes *input, int64_t inode) {
    char path[108]; struct stat st;
    close(fd);
    if (path_string(input, path, sizeof(path)) && !lstat(path, &st) && S_ISSOCK(st.st_mode) &&
        st.st_uid == geteuid() && (int64_t)st.st_ino == inode) unlink(path);
}

int32_t mc_owner_accept(int32_t listener) {
    int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return -errno;
    struct ucred credentials; socklen_t size = sizeof(credentials);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) ||
        size != sizeof(credentials) || credentials.uid != geteuid()) {
        close(fd); return -EACCES;
    }
    return fd;
}

int32_t mc_owner_connect(const Bytes *input) {
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    struct stat st;
    if (!path_string(input, address.sun_path, sizeof(address.sun_path))) return -ENAMETOOLONG;
    if (lstat(address.sun_path, &st)) return -errno;
    if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0777) != 0600) return -EACCES;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -errno;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address))) {
        int error = errno; close(fd); return -error;
    }
    struct ucred credentials; socklen_t size = sizeof(credentials);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) ||
        size != sizeof(credentials) || credentials.uid != geteuid()) {
        close(fd); return -EACCES;
    }
    return fd;
}

Bytes mc_owner_receive_bounded(int32_t fd, int32_t maximum) {
    uint8_t buffer[65537] = {0};
    ssize_t size = 0;
    if (maximum < 1 || maximum > 65536) buffer[0] = 3;
    else size = recv(fd, buffer + 1, (size_t)maximum, MSG_DONTWAIT | MSG_TRUNC);
    if (size > maximum) { size = 0; buffer[0] = 3; }
    else if (size > 0) buffer[0] = 1;
    else if (size == 0 && !buffer[0]) buffer[0] = 2;
    else if (size < 0) { size = 0; buffer[0] = errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : 3; }
    Bytes result = {hew_bytes_new((uint32_t)size + 1), 0, (uint32_t)size + 1};
    memcpy(result.ptr, buffer, result.len);
    return result;
}

Bytes mc_owner_receive(int32_t fd) {
    return mc_owner_receive_bounded(fd, 4096);
}
