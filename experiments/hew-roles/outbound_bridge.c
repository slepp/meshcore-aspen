#define _POSIX_C_SOURCE 200809L
#include "event_bridge.h"
#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/stat.h>

typedef struct { uint8_t *ptr; uint32_t offset, len; } Bytes;
extern uint8_t *hew_bytes_new(uint32_t);
typedef struct {
    CURL *easy;
    CURLM *multi;
    int ready;
    int error;
    int websocket;
    int subprotocol;
    size_t header_bytes;
    size_t write_remaining;
    struct curl_slist *headers;
    int64_t events;
    struct { int fd; int64_t key; dev_t device; ino_t inode; } watched[16];
    size_t watches;
} Outbound;

static pthread_once_t initialized = PTHREAD_ONCE_INIT;
static int initialization_result;
static void initialize(void) {
    initialization_result = curl_global_init(CURL_GLOBAL_DEFAULT);
}
static char *copy_string(const Bytes *value) {
    if (value->len > 4096 || (value->len && memchr(value->ptr + value->offset, 0, value->len)))
        return NULL;
    char *out = malloc((size_t)value->len + 1);
    if (!out) return NULL;
    if (value->len) memcpy(out, value->ptr + value->offset, value->len);
    out[value->len] = 0;
    return out;
}
static int allowed_url(const char *text) {
    CURLU *url = curl_url();
    char *host = NULL, *scheme = NULL, *user = NULL, *password = NULL;
    char *query = NULL, *fragment = NULL;
    int ok = url && curl_url_set(url, CURLUPART_URL, text, 0) == CURLUE_OK &&
        curl_url_get(url, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
        curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK;
    if (ok) {
        ok = (!strcmp(scheme, "http") || !strcmp(scheme, "https") ||
              !strcmp(scheme, "ws") || !strcmp(scheme, "wss")) &&
            (!strcasecmp(host, "localhost") || !strcmp(host, "127.0.0.1") ||
             !strcmp(host, "[::1]") || !strcasecmp(host, "mqtt-meshcore-1.ve6slp.ca")) &&
            curl_url_get(url, CURLUPART_USER, &user, 0) != CURLUE_OK &&
            curl_url_get(url, CURLUPART_PASSWORD, &password, 0) != CURLUE_OK &&
            curl_url_get(url, CURLUPART_QUERY, &query, 0) != CURLUE_OK &&
            curl_url_get(url, CURLUPART_FRAGMENT, &fragment, 0) != CURLUE_OK;
    }
    curl_free(host); curl_free(scheme); curl_free(user); curl_free(password);
    curl_free(query); curl_free(fragment);
    curl_url_cleanup(url);
    return ok;
}
void mc_outbound_close(int64_t handle) {
    Outbound *out = (Outbound *)(uintptr_t)handle;
    if (!out) return;
    for (size_t i = 0; i < out->watches; i++) {
        int error = mc_events_remove(out->events, out->watched[i].key);
        if (error) fprintf(stderr, "OUTBOUND_READINESS_FAILED stage=close errno=%d\n", -error);
    }
    if (out->events) mc_events_release(out->events);
    if (out->multi && out->easy) curl_multi_remove_handle(out->multi, out->easy);
    if (out->easy) curl_easy_cleanup(out->easy);
    if (out->multi) curl_multi_cleanup(out->multi);
    curl_slist_free_all(out->headers);
    free(out);
}

/* Snapshot libcurl's requested socket interests, not POLLOUT on every connection.
   The caller schedules the returned libcurl timer alongside protocol deadlines. */
int64_t mc_outbound_watch(int64_t handle, int64_t group, int32_t flags) {
    Outbound *out = (Outbound *)(uintptr_t)handle;
    if (!out || !group || flags < 1 || flags > 3 || (out->events && out->events != group)) return -EINVAL;
    if (!out->events) { mc_events_retain(group); out->events = group; }
    fd_set read, write, error;
    FD_ZERO(&read); FD_ZERO(&write); FD_ZERO(&error);
    int maximum = -1;
    long timeout = -1;
    if (out->ready) {
        curl_socket_t fd = CURL_SOCKET_BAD;
        if (curl_easy_getinfo(out->easy, CURLINFO_ACTIVESOCKET, &fd) != CURLE_OK ||
            fd == CURL_SOCKET_BAD || fd < 0 || fd >= FD_SETSIZE) return -EIO;
        maximum = (int)fd;
        if (flags & 1) FD_SET(fd, &read);
        if (flags & 2) FD_SET(fd, &write);
    } else if (curl_multi_fdset(out->multi, &read, &write, &error, &maximum) != CURLM_OK ||
               curl_multi_timeout(out->multi, &timeout) != CURLM_OK) return -EIO;
    for (size_t i = 0; i < out->watches;) {
        int fd = out->watched[i].fd;
        struct stat current;
        if (fd > maximum || (!FD_ISSET(fd, &read) && !FD_ISSET(fd, &write) && !FD_ISSET(fd, &error)) ||
            fstat(fd, &current) || current.st_dev != out->watched[i].device || current.st_ino != out->watched[i].inode) {
            int result = mc_events_remove(group, out->watched[i].key);
            if (result) return result;
            out->watched[i] = out->watched[--out->watches];
        } else i++;
    }
    for (int fd = 0; fd <= maximum; fd++) {
        int wanted = (FD_ISSET(fd, &read) || FD_ISSET(fd, &error) ? 1 : 0) |
            (FD_ISSET(fd, &write) ? 2 : 0);
        if (!wanted) continue;
        size_t index = 0;
        while (index < out->watches && out->watched[index].fd != fd) index++;
        if (index == out->watches) {
            if (out->watches == 16) return -EMFILE;
            int64_t key = mc_events_add(group, fd, wanted);
            if (key < 0) return key;
            struct stat current;
            if (fstat(fd, &current)) {
                int saved = errno;
                int result = mc_events_remove(group, key);
                if (result) fprintf(stderr, "OUTBOUND_READINESS_FAILED stage=register-cleanup errno=%d\n", -result);
                return -saved;
            }
            out->watched[index].fd = fd; out->watched[index].key = key;
            out->watched[index].device = current.st_dev; out->watched[index].inode = current.st_ino;
            out->watches++;
        } else {
            int result = mc_events_arm(group, out->watched[index].key, wanted);
            if (result) return result;
        }
    }
    /* Threaded DNS can temporarily have no socket; libcurl's resolver timer
       normally supplies the next step. Keep its documented empty-set backstop
       limited to connection establishment, never an idle connected socket. */
    if (!out->ready && maximum < 0 && timeout < 0) timeout = 100;
    return timeout < 0 ? INT64_MAX : (int64_t)timeout;
}

static size_t websocket_header(char *data, size_t size, size_t count, void *context) {
    Outbound *out = context;
    if (size && count > SIZE_MAX / size) return 0;
    size_t length = size * count;
    if (length > 16384 - out->header_bytes) return 0;
    out->header_bytes += length;
    const char prefix[] = "Sec-WebSocket-Protocol:";
    if (length >= sizeof prefix - 1 && !strncasecmp(data, prefix, sizeof prefix - 1)) {
        size_t start = sizeof prefix - 1, end = length;
        while (start < end && (data[start] == ' ' || data[start] == '\t')) start++;
        while (end > start && (data[end-1] == '\r' || data[end-1] == '\n' ||
                              data[end-1] == ' ' || data[end-1] == '\t')) end--;
        if (out->subprotocol || end - start != 4 || memcmp(data + start, "mqtt", 4))
            out->subprotocol = -1;
        else out->subprotocol = 1;
    }
    return length;
}

/* The multi API progresses DNS/TCP/TLS and optional WebSocket upgrade.
 * libcurl owns WebSocket framing; all MQTT bytes and policy remain in Hew. */
int64_t mc_outbound_new(const Bytes *url, const Bytes *ca) {
    pthread_once(&initialized, initialize);
    if (initialization_result ||
        !(curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_ASYNCHDNS)) return 0;
    char *address = copy_string(url), *certificate = copy_string(ca);
    if (!address || !certificate || !allowed_url(address)) {
        free(address); free(certificate); return 0;
    }
    Outbound *out = calloc(1, sizeof *out);
    if (!out) { free(address); free(certificate); return 0; }
    out->easy = curl_easy_init();
    out->multi = curl_multi_init();
    out->websocket = !strncasecmp(address, "ws:", 3) || !strncasecmp(address, "wss:", 4);
    int ok = out->easy && out->multi;
#define SET(option, value) do { if (ok && curl_easy_setopt(out->easy, option, value) != CURLE_OK) ok = 0; } while (0)
    SET(CURLOPT_URL, address);
    SET(CURLOPT_PROTOCOLS_STR, "http,https,ws,wss");
    SET(CURLOPT_PROXY, "");
    SET(CURLOPT_CONNECT_ONLY, out->websocket ? 2L : 1L);
    SET(CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    SET(CURLOPT_TIMEOUT_MS, 3000L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    if (*certificate) SET(CURLOPT_CAINFO, certificate);
    if (out->websocket) {
        out->headers = curl_slist_append(NULL, "Sec-WebSocket-Protocol: mqtt");
        if (!out->headers) ok = 0;
        SET(CURLOPT_HTTPHEADER, out->headers);
        SET(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        SET(CURLOPT_PATH_AS_IS, 1L);
        SET(CURLOPT_HEADERFUNCTION, websocket_header);
        SET(CURLOPT_HEADERDATA, out);
    }
#undef SET
    free(address); free(certificate);
    if (!ok || curl_multi_add_handle(out->multi, out->easy) != CURLM_OK) {
        mc_outbound_close((int64_t)(uintptr_t)out); return 0;
    }
    return (int64_t)(uintptr_t)out;
}
int32_t mc_outbound_step(int64_t handle) {
    Outbound *out = (Outbound *)(uintptr_t)handle;
    if (!out) return -CURLE_BAD_FUNCTION_ARGUMENT;
    if (out->error) return -out->error;
    if (out->ready) return 1;
    int running;
    CURLMcode result = curl_multi_perform(out->multi, &running);
    if (result != CURLM_OK) {
        out->error = 1000 + result; return -out->error;
    }
    int count;
    CURLMsg *message;
    while ((message = curl_multi_info_read(out->multi, &count))) {
        if (message->msg == CURLMSG_DONE && message->easy_handle == out->easy) {
            if (message->data.result != CURLE_OK) {
                out->error = message->data.result; return -out->error;
            }
            if (out->websocket && out->subprotocol != 1) {
                out->error = CURLE_WEIRD_SERVER_REPLY; return -out->error;
            }
            out->ready = 1; return 1;
        }
    }
    return 0;
}
int32_t mc_outbound_write(int64_t handle, const Bytes *data) {
    Outbound *out = (Outbound *)(uintptr_t)handle;
    if (!out || !out->ready || data->len > 65536) return -CURLE_BAD_FUNCTION_ARGUMENT;
    size_t sent = 0;
    CURLcode code;
    if (out->websocket) {
        if (!out->write_remaining) out->write_remaining = data->len;
        size_t size = data->len < out->write_remaining ? data->len : out->write_remaining;
        code = curl_ws_send(out->easy, data->ptr + data->offset, size, &sent, 0, CURLWS_BINARY);
        out->write_remaining -= sent;
    } else {
        code = curl_easy_send(out->easy, data->ptr + data->offset, data->len, &sent);
    }
    if (sent) return (int32_t)sent;
    if (code == CURLE_AGAIN) return 0;
    return code == CURLE_OK ? 0 : -(int32_t)code;
}
Bytes mc_outbound_read(int64_t handle) {
    Outbound *out = (Outbound *)(uintptr_t)handle;
    uint8_t buffer[4097] = {0};
    size_t received = 0, size = 1;
    CURLcode code = CURLE_BAD_FUNCTION_ARGUMENT;
    int end = 0;
    if (out && out->ready && out->websocket) {
        const struct curl_ws_frame *meta = NULL;
        code = curl_ws_recv(out->easy, buffer + 1, sizeof buffer - 1, &received, &meta);
        if (code == CURLE_GOT_NOTHING) {
            code = CURLE_OK; end = 1;
        } else if (code == CURLE_OK && meta) {
            if (meta->flags & CURLWS_CLOSE) {
                received = 0; end = 1;
            } else if (meta->flags & (CURLWS_PING | CURLWS_PONG)) {
                received = 0; code = CURLE_AGAIN;
            } else if (!(meta->flags & CURLWS_BINARY)) {
                received = 0; code = CURLE_RECV_ERROR;
            } else if (meta->offset < 0 || meta->bytesleft < 0 ||
                       meta->offset > 1048576 || received > (size_t)(1048576 - meta->offset) ||
                       meta->bytesleft > 1048576 - meta->offset - (curl_off_t)received) {
                received = 0; code = CURLE_TOO_LARGE;
            }
        }
    } else if (out && out->ready) {
        code = curl_easy_recv(out->easy, buffer + 1, sizeof buffer - 1, &received);
        end = code == CURLE_OK && !received;
    }
    if (code == CURLE_OK) {
        buffer[0] = received ? 1 : (end ? 2 : 0);
        size += received;
    } else if (code != CURLE_AGAIN) {
        buffer[0] = 3; size = 5;
        for (unsigned i = 0; i < 4; i++) buffer[i + 1] = (uint8_t)((uint32_t)code >> (i * 8));
    }
    Bytes result = {hew_bytes_new((uint32_t)size), 0, (uint32_t)size};
    memcpy(result.ptr, buffer, size);
    return result;
}
