/* Exercise the production POSIX bridge without a Hew compiler/runtime dependency. */
#include "../storage_bridge.c"

uint8_t *hew_bytes_new(uint32_t size) { return malloc(size); }

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    Bytes path = {(uint8_t *)argv[2], 0, (uint32_t)strlen(argv[2])};
    Bytes data = {(uint8_t *)"snapshot", 0, 8};
    if (!strcmp(argv[1], "read")) {
        Bytes result = mc_read_bounded(&path, 65536);
        printf("%u\n", result.len);
        free(result.ptr);
    } else if (!strcmp(argv[1], "lock")) {
        int32_t lock = mc_lock(&path);
        printf("%d\n", lock < 0 ? -1 : 0);
        mc_unlock(lock);
    } else if (!strcmp(argv[1], "append")) {
        printf("%d\n", mc_append_private(&path, &data, 65536));
    } else if (!strcmp(argv[1], "commit")) {
        printf("%d\n", mc_commit_bounded(&path, &data, 65536));
    } else if (!strcmp(argv[1], "recover")) {
        char guard[4096];
        if (snprintf(guard, sizeof(guard), "%s.guard", argv[2]) >= (int)sizeof(guard)) return 2;
        Bytes guard_path = {(uint8_t *)guard, 0, (uint32_t)strlen(guard)};
        int32_t lock = mc_lock(&guard_path);
        if (lock < 0) return 2;
        printf("%d\n", mc_recover(&path, lock));
        mc_unlock(lock);
    } else return 2;
    return 0;
}
