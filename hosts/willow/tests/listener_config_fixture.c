/* Exercise the production configuration reader without starting a listener. */
#include "../native/listener_bridge.c"

uint8_t *hew_bytes_new(uint32_t size) { return malloc(size); }

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    Bytes path = {(uint8_t *)argv[1], 0, (uint32_t)strlen(argv[1])};
    Bytes result = hew_listener_config(&path);
    int ok = fwrite(result.ptr, 1, result.len, stdout) == result.len;
    free(result.ptr);
    return ok ? 0 : 2;
}
