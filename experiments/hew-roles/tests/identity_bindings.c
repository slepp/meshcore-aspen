#include <stdio.h>
#include "../crypto_bridge.c"

uint8_t *hew_bytes_new(uint32_t capacity) { return malloc(capacity); }

int main(void) {
    uint8_t raw[64], peer[32], text[] = "Willow key check";
    int size = getchar();
    if ((size != 32 && size != 64) || fread(raw, 1, size, stdin) != (size_t)size
        || fread(peer, 1, 32, stdin) != 32 || getchar() != EOF) return 1;
    Bytes key = {raw, 0, size}, remote = {peer, 0, 32}, message = {text, 0, 16};
    Bytes public = mc_public(&key), signature = mc_sign(&key, &message), shared = mc_shared(&key, &remote);
    if (public.len != 32 || signature.len != 64 || shared.len != 32) return 2;
    Bytes sealed = mc_aes(&shared, &message, 1);
    if (sealed.len != 16 || fwrite(data(&public), 1, 32, stdout) != 32
        || fwrite(data(&signature), 1, 64, stdout) != 64
        || fwrite(data(&sealed), 1, 16, stdout) != 16) return 3;
    OPENSSL_cleanse(raw, sizeof raw);
    OPENSSL_cleanse(shared.ptr, shared.len);
    free(public.ptr);
    free(signature.ptr);
    free(shared.ptr);
    free(sealed.ptr);
    return 0;
}
