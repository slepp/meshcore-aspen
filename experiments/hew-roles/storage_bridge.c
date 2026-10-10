#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifdef MC_STORAGE_TEST_INTERRUPT
#include <signal.h>
#endif

typedef struct { uint8_t *ptr; uint32_t offset, len; } Bytes;
extern uint8_t *hew_bytes_new(uint32_t);
static int path_string(const Bytes *in, char out[4096]) {
    if (!in->len || in->len > 4000 || !in->ptr) return 0;
    memcpy(out, in->ptr + in->offset, in->len);
    if (memchr(out, 0, in->len)) return 0;
    out[in->len] = 0;
    return 1;
}
Bytes mc_storage_usage(const Bytes *path) {
    char name[4096];
    struct statvfs info;
    if (!path_string(path, name) || statvfs(name, &info) || !info.f_frsize ||
        info.f_bfree > info.f_blocks) return (Bytes){0};
    const uint64_t maximum = (uint64_t)UINT32_MAX * 1024 + 1023;
    if (info.f_blocks > maximum / info.f_frsize) return (Bytes){0};
    uint32_t total = (uint32_t)((uint64_t)info.f_blocks * info.f_frsize / 1024);
    uint32_t used = (uint32_t)((uint64_t)(info.f_blocks - info.f_bfree) * info.f_frsize / 1024);
    uint8_t *out = hew_bytes_new(8);
    if (!out) return (Bytes){0};
    for (int i = 0; i < 4; ++i) {
        out[i] = (uint8_t)(used >> (i * 8));
        out[i + 4] = (uint8_t)(total >> (i * 8));
    }
    return (Bytes){out, 0, 8};
}
/* These are POSIX file operations only. Hew owns the snapshot format/state.
 * -1: definite failure; -2: rename succeeded but directory sync failed. */
int32_t mc_commit_bounded(const Bytes *path, const Bytes *data, int32_t limit) {
    char target[4096], pending[4096], parent[4096];
    if (!path_string(path, target) || limit <= 0 || limit > 16777216 ||
        data->len > (uint32_t)limit) return -1;
    strcpy(pending, target); strcat(pending, ".pending");
    strcpy(parent, target);
    char *slash = strrchr(parent, '/');
    if (slash) { if (slash == parent) slash[1] = 0; else *slash = 0; }
    else strcpy(parent, ".");
    int directory = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return -1;
    int fd = open(pending, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { close(directory); return -1; }
#ifdef MC_STORAGE_TEST_INTERRUPT
    const char *interrupt = getenv("HEW_TEST_COMMIT_INTERRUPT");
    if (interrupt && !strcmp(interrupt, target)) {
        if (write(fd, data->ptr + data->offset, data->len / 2) < 0 || fsync(fd)) {
            close(fd); close(directory); return -1;
        }
        raise(SIGSTOP);
        if (lseek(fd, 0, SEEK_SET) < 0) { close(fd); close(directory); return -1; }
    }
#endif
    size_t at = 0;
    while (at < data->len) {
        ssize_t n = write(fd, data->ptr + data->offset + at, data->len - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        at += (size_t)n;
    }
    int ok = at == data->len && fsync(fd) == 0;
    if (close(fd)) ok = 0;
    if (!ok || rename(pending, target)) {
        unlink(pending); close(directory); return -1;
    }
    int result = fsync(directory) == 0 ? 0 : -2;
    close(directory);
    return result;
}
int32_t mc_commit(const Bytes *path, const Bytes *data) {
    return mc_commit_bounded(path, data, 65536);
}
/* The caller holds the service or per-target lock covering this path.
 * Recovery only removes a private abandoned staging file; never promotes it. */
int32_t mc_recover(const Bytes *path, int32_t lock) {
    char target[4096], parent[4096], pending[4096];
    struct stat held, directory_stat, staged;
    if (!path_string(path, target) || fstat(lock, &held) || !S_ISREG(held.st_mode) ||
        held.st_uid != geteuid() || (held.st_mode & 077) ||
        flock(lock, LOCK_EX | LOCK_NB)) return -1;
    strcpy(parent, target);
    char *slash = strrchr(parent, '/');
    const char *leaf = strrchr(target, '/');
    leaf = leaf ? leaf + 1 : target;
    if (!*leaf || !strcmp(leaf, ".") || !strcmp(leaf, "..")) return -1;
    strcpy(pending, leaf); strcat(pending, ".pending");
    if (slash) { if (slash == parent) slash[1] = 0; else *slash = 0; }
    else strcpy(parent, ".");
    int directory = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return -1;
    if (fstatat(directory, pending, &staged, AT_SYMLINK_NOFOLLOW)) {
        int result = errno == ENOENT ? 0 : -1;
        close(directory); return result;
    }
    if (fstat(directory, &directory_stat) || directory_stat.st_uid != geteuid() ||
        (directory_stat.st_mode & 022) || !S_ISREG(staged.st_mode) || staged.st_uid != geteuid() ||
        (staged.st_mode & 07777) != 0600 || staged.st_nlink != 1) {
        close(directory); return -1;
    }
    int result = unlinkat(directory, pending, 0) == 0 && fsync(directory) == 0 ? 0 : -1;
    close(directory);
    return result;
}
Bytes mc_read_bounded(const Bytes *path, int32_t limit) {
    char name[4096];
    if (!path_string(path, name) || limit <= 0 || limit > 16777216) return (Bytes){0};
    int fd = open(name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return (Bytes){0};
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 077) ||
        st.st_uid != geteuid() || st.st_size <= 0 || st.st_size > limit) {
        close(fd); return (Bytes){0};
    }
    uint8_t *data = malloc((size_t)st.st_size);
    if (!data) { close(fd); return (Bytes){0}; }
    size_t at = 0;
    while (at < (size_t)st.st_size) {
        ssize_t n = read(fd, data + at, (size_t)st.st_size - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        at += (size_t)n;
    }
    close(fd);
    if (at != (size_t)st.st_size) { free(data); return (Bytes){0}; }
    Bytes out = {hew_bytes_new((uint32_t)at), 0, (uint32_t)at};
    memcpy(out.ptr, data, at);
    free(data);
    return out;
}
Bytes mc_read(const Bytes *path) { return mc_read_bounded(path, 65536); }
int32_t mc_lock(const Bytes *path) {
    char name[4096];
    if (!path_string(path, name)) return -1;
    strcat(name, ".lock");
    int fd = open(name, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077) || flock(fd, LOCK_EX | LOCK_NB)) {
        close(fd); return -1;
    }
    return fd;
}
void mc_unlock(int32_t fd) { if (fd >= 0) close(fd); }
int32_t mc_absent(const Bytes *path) {
    char name[4096];
    struct stat st;
    if (!path_string(path, name)) return 0;
    return lstat(name, &st) < 0 && errno == ENOENT;
}

static int private_parent(const Bytes *path, char leaf[4096]) {
    char target[4096];
    if (!path_string(path, target)) return -1;
    char *slash = strrchr(target, '/');
    const char *name = slash ? slash + 1 : target;
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) return -1;
    strcpy(leaf, name);
    if (slash) { if (slash == target) slash[1] = 0; else *slash = 0; }
    else strcpy(target, ".");
    int directory = open(target, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (directory < 0) return -1;
    if (fstat(directory, &st) || st.st_uid != geteuid() || (st.st_mode & 077)) {
        close(directory); return -1;
    }
    return directory;
}

/* 1: bound reached without writing; -2: a write/unlink may have committed. */
int32_t mc_append_private(const Bytes *path, const Bytes *data, int64_t limit) {
    char leaf[4096];
    if (data->len > 4096 || limit < 1024 || limit > 16 * 1024 * 1024) return -1;
    int directory = private_parent(path, leaf);
    if (directory < 0) return -1;
    int fd = openat(directory, leaf, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd < 0) { close(directory); return -1; }
    struct stat st;
    int result = -1;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
        !(st.st_mode & 077) && st.st_nlink == 1 && st.st_size >= 0) {
        if (st.st_size >= limit || data->len > (uint64_t)(limit - st.st_size)) result = 1;
        else {
            size_t at = 0;
            while (at < data->len) {
                ssize_t n = write(fd, data->ptr + data->offset + at, data->len - at);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                at += (size_t)n;
            }
            result = at == data->len && fsync(fd) == 0 && fsync(directory) == 0 ? 0 : -2;
        }
    }
    if (close(fd) && result == 0) result = -2;
    close(directory);
    return result;
}

/* Removes an empty directory this user owns with no group/other access. */
int32_t mc_remove_private_directory(const Bytes *path) {
    char name[4096];
    struct stat st;
    if (!path_string(path, name)) return -1;
    if (lstat(name, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077)) return -1;
    return rmdir(name) ? -1 : 0;
}

int32_t mc_erase_private(const Bytes *path) {
    char leaf[4096];
    int directory = private_parent(path, leaf);
    if (directory < 0) return -1;
    struct stat st;
    int result;
    if (fstatat(directory, leaf, &st, AT_SYMLINK_NOFOLLOW)) result = errno == ENOENT ? 0 : -1;
    else if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) || st.st_nlink != 1) result = -1;
    else if (unlinkat(directory, leaf, 0)) result = -1;
    else result = fsync(directory) == 0 ? 0 : -2;
    close(directory);
    return result;
}
