#include "kernel/entropy.h"
#include "kernel/fs.h"

/* Event timings are folded into a small pool; every fill hashes the pool and
 * the TSC into the key, draws ChaCha20 blocks and then replaces the key with
 * generator output, so earlier output cannot be rebuilt from a later key. */
static uint32_t pool[8];
static uint32_t key[8];
static uint32_t counter;
static unsigned pool_slot;

static uint64_t tsc(void) {
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return (uint64_t)high << 32 | low;
}

static uint32_t rotl(uint32_t v, unsigned n) { return v << n | v >> (32 - n); }

#define QUARTER(a, b, c, d) \
    a += b; d = rotl(d ^ a, 16); c += d; b = rotl(b ^ c, 12); \
    a += b; d = rotl(d ^ a, 8);  c += d; b = rotl(b ^ c, 7)

static void chacha_block(const uint32_t in[16], uint32_t out[16]) {
    uint32_t x[16];
    for (int i = 0; i < 16; ++i) x[i] = in[i];
    for (int i = 0; i < 10; ++i) {
        QUARTER(x[0], x[4], x[8], x[12]); QUARTER(x[1], x[5], x[9], x[13]);
        QUARTER(x[2], x[6], x[10], x[14]); QUARTER(x[3], x[7], x[11], x[15]);
        QUARTER(x[0], x[5], x[10], x[15]); QUARTER(x[1], x[6], x[11], x[12]);
        QUARTER(x[2], x[7], x[8], x[13]); QUARTER(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; ++i) out[i] = x[i] + in[i];
}

static void block(uint32_t out[16], uint32_t nonce) {
    uint32_t in[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; ++i) in[4 + i] = key[i];
    in[12] = counter++;
    in[13] = nonce;
    uint64_t now = tsc();
    in[14] = (uint32_t)now;
    in[15] = (uint32_t)(now >> 32);
    chacha_block(in, out);
}

void entropy_mix(uint32_t value) {
    uint64_t now = tsc();
    unsigned slot = pool_slot++ & 7;
    pool[slot] = rotl(pool[slot], 7) ^ (uint32_t)now ^ value * 0x9e3779b9u;
}

void entropy_fill(void *out, uint32_t length) {
    uint32_t words[16];
    /* Fold the pool and fresh timing into the key. */
    for (int i = 0; i < 8; ++i) key[i] ^= pool[i];
    for (int i = 0; i < 4; ++i) entropy_mix((uint32_t)i);
    block(words, 1);
    for (int i = 0; i < 8; ++i) key[i] ^= words[i];
    uint8_t *bytes = out;
    while (length) {
        block(words, 0);
        uint32_t n = length < sizeof words ? length : sizeof words;
        kmemcpy(bytes, words, (int)n);
        bytes += n;
        length -= n;
    }
    block(words, 2);
    for (int i = 0; i < 8; ++i) key[i] = words[i];
    kmemset(words, 0, sizeof words);
}
