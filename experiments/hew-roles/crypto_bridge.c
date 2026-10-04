#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>

/* Hew rc7 bytes ABI. Inputs are borrowed, outputs use the runtime allocator.
 * No role, routing, framing, or protocol state lives on this side of the ABI. */
typedef struct { uint8_t *ptr; uint32_t offset, len; } Bytes;
extern uint8_t *hew_bytes_new(uint32_t capacity);
extern int crypto_sign_seed_keypair(unsigned char *, unsigned char *, const unsigned char *);
extern int crypto_sign_ed25519_pk_to_curve25519(unsigned char *, const unsigned char *);
extern int crypto_sign_ed25519_sk_to_curve25519(unsigned char *, const unsigned char *);
extern int crypto_scalarmult_curve25519(unsigned char *, const unsigned char *, const unsigned char *);
extern int crypto_sign_detached(unsigned char *, unsigned long long *,
                                const unsigned char *, unsigned long long, const unsigned char *);
extern int crypto_scalarmult_ed25519_base_noclamp(unsigned char *, const unsigned char *);
extern void crypto_core_ed25519_scalar_reduce(unsigned char *, const unsigned char *);
extern void crypto_core_ed25519_scalar_mul(unsigned char *, const unsigned char *, const unsigned char *);
extern void crypto_core_ed25519_scalar_add(unsigned char *, const unsigned char *, const unsigned char *);

static const uint8_t *data(const Bytes *b) { return b->ptr ? b->ptr + b->offset : NULL; }
static Bytes copy(const uint8_t *p, uint32_t len) {
    if (!len) return (Bytes){0};
    Bytes out = {hew_bytes_new(len), 0, len};
    memcpy(out.ptr, p, len);
    return out;
}
static int expanded_valid(const Bytes *key) {
    return key->len == 64 && !(data(key)[0] & 7) && (data(key)[31] & 0xc0) == 0x40;
}
static int sha512_parts(uint8_t out[64], const uint8_t *a, size_t alen,
                        const uint8_t *b, size_t blen, const uint8_t *c, size_t clen) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int size = 0;
    int ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha512(), NULL) == 1
        && EVP_DigestUpdate(ctx, a, alen) == 1
        && EVP_DigestUpdate(ctx, b, blen) == 1
        && EVP_DigestUpdate(ctx, c, clen) == 1
        && EVP_DigestFinal_ex(ctx, out, &size) == 1 && size == 64;
    EVP_MD_CTX_free(ctx);
    return ok;
}
/* Native MeshCore keys store the clamped scalar and nonce prefix, not a seed
 * or libsodium's seed+public secret-key encoding. RFC 8032 signing uses both. */
static Bytes sign_expanded(const Bytes *key, const Bytes *message) {
    uint8_t pk[32], signature[64], wide[64] = {0}, scalar[32], nonce[32], challenge[32], product[32];
    int ok = expanded_valid(key) && !crypto_scalarmult_ed25519_base_noclamp(pk, data(key));
    if (ok) {
        memcpy(wide, data(key), 32);
        crypto_core_ed25519_scalar_reduce(scalar, wide);
        ok = sha512_parts(wide, data(key) + 32, 32, data(message), message->len, NULL, 0);
    }
    if (ok) {
        crypto_core_ed25519_scalar_reduce(nonce, wide);
        ok = !crypto_scalarmult_ed25519_base_noclamp(signature, nonce)
            && sha512_parts(wide, signature, 32, pk, 32, data(message), message->len);
    }
    if (ok) {
        crypto_core_ed25519_scalar_reduce(challenge, wide);
        crypto_core_ed25519_scalar_mul(product, challenge, scalar);
        crypto_core_ed25519_scalar_add(signature + 32, product, nonce);
    }
    Bytes result = ok ? copy(signature, 64) : (Bytes){0};
    OPENSSL_cleanse(wide, sizeof wide);
    OPENSSL_cleanse(scalar, sizeof scalar);
    OPENSSL_cleanse(nonce, sizeof nonce);
    OPENSSL_cleanse(product, sizeof product);
    return result;
}
Bytes mc_public(const Bytes *seed) {
    uint8_t pk[32], sk[64];
    if (seed->len == 64) {
        if (!expanded_valid(seed) || crypto_scalarmult_ed25519_base_noclamp(pk, data(seed))) return (Bytes){0};
        return copy(pk, sizeof pk);
    }
    if (seed->len != 32 || crypto_sign_seed_keypair(pk, sk, data(seed))) return (Bytes){0};
    OPENSSL_cleanse(sk, sizeof sk);
    return copy(pk, sizeof pk);
}
Bytes mc_sign(const Bytes *seed, const Bytes *message) {
    if (seed->len == 64) return sign_expanded(seed, message);
    uint8_t pk[32], sk[64], signature[64];
    unsigned long long size = 0;
    if (seed->len != 32 || crypto_sign_seed_keypair(pk, sk, data(seed))) return (Bytes){0};
    int failed = crypto_sign_detached(signature, &size, data(message), message->len, sk);
    OPENSSL_cleanse(sk, sizeof sk);
    return failed || size != 64 ? (Bytes){0} : copy(signature, 64);
}
Bytes mc_expanded(const Bytes *key) {
    if (key->len == 64) return expanded_valid(key) ? copy(data(key), 64) : (Bytes){0};
    uint8_t expanded[64];
    if (key->len != 32 || !sha512_parts(expanded, data(key), 32, NULL, 0, NULL, 0))
        return (Bytes){0};
    expanded[0] &= 248;
    expanded[31] &= 63;
    expanded[31] |= 64;
    Bytes result = copy(expanded, 64);
    OPENSSL_cleanse(expanded, sizeof expanded);
    return result;
}
Bytes mc_shared(const Bytes *seed, const Bytes *peer) {
    uint8_t pk[32], sk[64], scalar[32], point[32], shared[32];
    int failed = peer->len != 32;
    if (seed->len == 64) {
        failed |= !expanded_valid(seed);
        if (!failed) memcpy(scalar, data(seed), 32);
    } else {
        failed |= seed->len != 32;
        if (!failed) failed = crypto_sign_seed_keypair(pk, sk, data(seed));
        if (!failed) failed = crypto_sign_ed25519_sk_to_curve25519(scalar, sk);
    }
    if (!failed) failed = crypto_sign_ed25519_pk_to_curve25519(point, data(peer));
    if (!failed) failed = crypto_scalarmult_curve25519(shared, scalar, point);
    Bytes out = failed ? (Bytes){0} : copy(shared, 32);
    OPENSSL_cleanse(sk, sizeof sk);
    OPENSSL_cleanse(scalar, sizeof scalar);
    OPENSSL_cleanse(shared, sizeof shared);
    return out;
}
Bytes mc_aes(const Bytes *key, const Bytes *input, int32_t encrypt) {
    if (key->len < 16 || !input->len || input->len > 256 || input->len % 16) return (Bytes){0};
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    uint8_t out[272];
    int length = 0, tail = 0;
    int ok = ctx && EVP_CipherInit_ex(ctx, EVP_aes_128_ecb(), NULL, data(key), NULL, encrypt) == 1
        && EVP_CIPHER_CTX_set_padding(ctx, 0) == 1
        && EVP_CipherUpdate(ctx, out, &length, data(input), (int)input->len) == 1
        && EVP_CipherFinal_ex(ctx, out + length, &tail) == 1;
    EVP_CIPHER_CTX_free(ctx);
    Bytes result = ok ? copy(out, (uint32_t)(length + tail)) : (Bytes){0};
    OPENSSL_cleanse(out, sizeof out);
    return result;
}
