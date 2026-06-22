/**
 * lxmf_stamp — LXMF LXStamper proof-of-work, self-contained.
 *
 * Software SHA-256 (FIPS 180-4) plus HMAC/HKDF-SHA256, kept off the
 * shared hardware SHA accelerator so the ~10^5-iteration search loop
 * doesn't starve link/TLS crypto on other tasks. The generator exploits
 * SHA-256's streaming structure: the 768 KB workblock is block-aligned,
 * so its compression state ("midstate") is computed once and each nonce
 * attempt only compresses the final 64-byte block — turning a ~50 GB
 * naive search into a few MB.
 */
#include "lxmf_stamp.h"
#include "mem.h"

#include <cstring>

namespace {

/* ── SHA-256 (FIPS 180-4) ── */

const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

struct Sha256 {
    uint32_t h[8];
    uint64_t len;        /* total bytes absorbed */
    uint8_t  buf[64];
    size_t   buf_len;
};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256_compress(uint32_t h[8], const uint8_t blk[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)blk[i*4] << 24) | ((uint32_t)blk[i*4+1] << 16) |
               ((uint32_t)blk[i*4+2] << 8) | (uint32_t)blk[i*4+3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=h[0], b=h[1], c=h[2], d=h[3], e=h[4], f=h[5], g=h[6], hh=h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

void sha256_init(Sha256& s)
{
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    std::memcpy(s.h, iv, sizeof iv);
    s.len = 0;
    s.buf_len = 0;
}

void sha256_update(Sha256& s, const uint8_t* p, size_t n)
{
    s.len += n;
    if (s.buf_len) {
        while (n && s.buf_len < 64) { s.buf[s.buf_len++] = *p++; --n; }
        if (s.buf_len == 64) { sha256_compress(s.h, s.buf); s.buf_len = 0; }
    }
    while (n >= 64) { sha256_compress(s.h, p); p += 64; n -= 64; }
    while (n) { s.buf[s.buf_len++] = *p++; --n; }
}

/* Takes `s` by value so the caller's running context (e.g. a cached
 * midstate) is left untouched — finishing is non-destructive. */
void sha256_final(Sha256 s, uint8_t out[32])
{
    uint64_t bits = s.len * 8;
    const uint8_t pad = 0x80, zero = 0x00;
    sha256_update(s, &pad, 1);
    while (s.buf_len != 56) sha256_update(s, &zero, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; ++i) lb[i] = (uint8_t)(bits >> (56 - 8*i));
    sha256_update(s, lb, 8);
    for (int i = 0; i < 8; ++i) {
        out[i*4]   = (uint8_t)(s.h[i] >> 24);
        out[i*4+1] = (uint8_t)(s.h[i] >> 16);
        out[i*4+2] = (uint8_t)(s.h[i] >> 8);
        out[i*4+3] = (uint8_t)(s.h[i]);
    }
}

void sha256(const uint8_t* p, size_t n, uint8_t out[32])
{
    Sha256 s; sha256_init(s); sha256_update(s, p, n); sha256_final(s, out);
}

/* HMAC-SHA256. Keys here are always <= 64 B (a salt or a PRK), but the
 * >64 path is handled for completeness. */
void hmac_sha256(const uint8_t* key, size_t klen,
                 const uint8_t* msg, size_t mlen, uint8_t out[32])
{
    uint8_t k[64] = {0};
    if (klen > 64) sha256(key, klen, k);
    else           std::memcpy(k, key, klen);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    uint8_t inner[32];
    Sha256 s;
    sha256_init(s); sha256_update(s, ipad, 64); sha256_update(s, msg, mlen); sha256_final(s, inner);
    sha256_init(s); sha256_update(s, opad, 64); sha256_update(s, inner, 32); sha256_final(s, out);
}

/* HKDF-SHA256 producing exactly 256 bytes (8 blocks), context/info empty
 * — the shape stamp_workblock() requests. */
void hkdf256(const uint8_t* ikm, size_t ikm_len,
             const uint8_t* salt, size_t salt_len, uint8_t out[256])
{
    uint8_t prk[32];
    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);   /* extract */
    uint8_t t[32];
    size_t  t_len = 0;
    for (int i = 0; i < 8; ++i) {                      /* expand */
        uint8_t data[33];
        size_t  dl = 0;
        if (t_len) { std::memcpy(data, t, 32); dl = 32; }
        data[dl++] = (uint8_t)(i + 1);                 /* counter */
        hmac_sha256(prk, 32, data, dl, t);
        t_len = 32;
        std::memcpy(out + i*32, t, 32);
    }
}

/* msgpack-encode a small non-negative int the way the reference's
 * msgpack.packb(n) does (minimal encoding). n stays well under 2^16. */
size_t packb_uint(int n, uint8_t out[3])
{
    if (n <= 0x7f) { out[0] = (uint8_t)n; return 1; }
    if (n <= 0xff) { out[0] = 0xcc; out[1] = (uint8_t)n; return 2; }
    out[0] = 0xcd; out[1] = (uint8_t)(n >> 8); out[2] = (uint8_t)n; return 3;
}

/* Protocol constant — must match the reference exactly so both ends
 * derive the same workblock. 3000 * 256 B = 768000 B (block-aligned). */
constexpr int    WORKBLOCK_EXPAND_ROUNDS = 3000;
constexpr size_t WORKBLOCK_LEN = (size_t)WORKBLOCK_EXPAND_ROUNDS * 256;

/* Expand message_id into the 768 KB workblock (caller frees with gp_free). */
uint8_t* build_workblock(const uint8_t message_id[32])
{
    uint8_t* wb = (uint8_t*)gp_alloc(WORKBLOCK_LEN);
    if (!wb) return nullptr;
    for (int n = 0; n < WORKBLOCK_EXPAND_ROUNDS; ++n) {
        uint8_t pb[3];
        size_t  pbl = packb_uint(n, pb);
        /* salt = SHA-256(message_id || msgpack(n)) */
        uint8_t saltbuf[35];
        std::memcpy(saltbuf, message_id, 32);
        std::memcpy(saltbuf + 32, pb, pbl);
        uint8_t salt[32];
        sha256(saltbuf, 32 + pbl, salt);
        hkdf256(message_id, 32, salt, 32, wb + (size_t)n * 256);
    }
    return wb;
}

/* True iff `digest` (big-endian 256-bit) <= 2^(256 - cost), matching the
 * reference stamp_valid() `int.from_bytes(result) > target` test. */
bool hash_meets_cost(const uint8_t digest[32], int cost)
{
    if (cost <= 0)   return true;
    if (cost >= 256) cost = 256;
    uint8_t target[32] = {0};
    int b = 256 - cost;                 /* 0..255: single set bit */
    target[31 - (b / 8)] = (uint8_t)(1u << (b % 8));
    return std::memcmp(digest, target, 32) <= 0;
}

} // namespace

bool lxmfStampValid(const uint8_t message_id[32], int target_cost,
                    const uint8_t* stamp, size_t stamp_len)
{
    if (target_cost <= 0) return true;
    if (!stamp || stamp_len == 0) return false;
    uint8_t* wb = build_workblock(message_id);
    if (!wb) return false;
    Sha256 s;
    sha256_init(s);
    sha256_update(s, wb, WORKBLOCK_LEN);
    sha256_update(s, stamp, stamp_len);
    uint8_t d[32];
    sha256_final(s, d);
    gp_free(wb);
    return hash_meets_cost(d, target_cost);
}

bool lxmfStampGenerate(const uint8_t message_id[32], int target_cost,
                       uint8_t out_stamp[LXMF_STAMP_LEN], void (*yield)(void))
{
    if (target_cost <= 0) return false;
    uint8_t* wb = build_workblock(message_id);
    if (!wb) return false;

    /* Absorb the whole (block-aligned) workblock once; `base` now holds
     * the midstate with an empty buffer. Each attempt clones it and
     * compresses only the final block holding the 32-byte stamp. */
    Sha256 base;
    sha256_init(base);
    sha256_update(base, wb, WORKBLOCK_LEN);
    gp_free(wb);

    uint8_t  stamp[LXMF_STAMP_LEN] = {0};
    uint64_t counter = 0;
    for (;;) {
        for (int i = 0; i < 8; ++i)
            stamp[LXMF_STAMP_LEN - 1 - i] = (uint8_t)(counter >> (8 * i));
        Sha256 s = base;                 /* clone midstate */
        sha256_update(s, stamp, LXMF_STAMP_LEN);
        uint8_t d[32];
        sha256_final(s, d);
        if (hash_meets_cost(d, target_cost)) {
            std::memcpy(out_stamp, stamp, LXMF_STAMP_LEN);
            return true;
        }
        ++counter;
        if (yield && (counter & 0xfff) == 0) yield();
    }
}
