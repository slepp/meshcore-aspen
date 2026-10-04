#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[1], "argv")) {
        if (strcmp(argv[2], "") || strcmp(argv[3], "two words") || strcmp(argv[4], "Willow-λ")) return 9;
        return write(STDOUT_FILENO, "ARGV_OK", 7) == 7 ? 0 : 10;
    }
    if (argc != 2) return 2;
    const char *name = strrchr(argv[1], '/');
    if (!name) return 3;
    if (!strcmp(name, "/exit7")) return 7;
    if (!strcmp(name, "/ignore-term")) {
        signal(SIGTERM, SIG_IGN);
        for (;;) pause();
    }
    char data[1024];
    ssize_t size;
    while ((size = read(STDIN_FILENO, data, sizeof data)) > 0) {
        ssize_t at = 0;
        while (at < size) {
            ssize_t sent = write(STDOUT_FILENO, data + at, (size_t)(size - at));
            if (sent <= 0) return 4;
            at += sent;
        }
    }
    return 0;
}
