/*
 * Boot-time LZMA1 decoder for the packed kernel payload.
 *
 * It runs from the staged copy below 1 MiB before the kernel is relocated, so
 * it must be position independent: one function, no static data, no jump
 * tables and no library calls. tests/test_kernel_pack.py checks the object has
 * no relocations. The caller supplies 16 KiB of scratch for probabilities.
 *
 * Original implementation of the LZMA format as described in the public LZMA
 * SDK specification (lzma-specification.txt). Fixed properties: lc=3, lp=0,
 * pb=2, which tools/kernel_pack.py also uses. The output size is known, so an
 * end marker after the last byte is accepted but not required.
 */
#include <stdint.h>

#define PROB_INIT 1024u
#define TOP (1u << 24)

typedef struct {
    uint16_t choice, choice2, low[4][8], mid[4][8], high[256];
} LenProbs;

typedef struct {
    uint16_t is_match[12][4], is_rep[12], rep_g0[12], rep_g1[12], rep_g2[12];
    uint16_t rep0_long[12][4], slot[4][64], special[115], align[16];
    LenProbs len, rep_len;
    uint16_t literal[8][0x300];
} BootLzmaProbs;

typedef struct {
    const uint8_t *in, *in_end;
    uint32_t range, code;
    int error;
} RangeDecoder;

#define INLINE static

INLINE void rc_normalize(RangeDecoder *rc) {
    if (rc->range < TOP) {
        rc->range <<= 8;
        if (rc->in == rc->in_end) { rc->error = 1; return; }
        rc->code = (rc->code << 8) | *rc->in++;
    }
}

static __attribute__((noinline)) unsigned rc_bit(RangeDecoder *rc, uint16_t *prob) {
    uint32_t bound = (rc->range >> 11) * *prob;
    unsigned bit;
    if (rc->code < bound) {
        rc->range = bound;
        *prob = (uint16_t)(*prob + ((2048u - *prob) >> 5));
        bit = 0;
    } else {
        rc->range -= bound;
        rc->code -= bound;
        *prob = (uint16_t)(*prob - (*prob >> 5));
        bit = 1;
    }
    rc_normalize(rc);
    return bit;
}

INLINE unsigned rc_direct(RangeDecoder *rc, unsigned count) {
    unsigned result = 0;
    while (count--) {
        rc->range >>= 1;
        unsigned bit = rc->code >= rc->range;
        if (bit) rc->code -= rc->range;
        result = (result << 1) | bit;
        rc_normalize(rc);
    }
    return result;
}

INLINE unsigned tree(RangeDecoder *rc, uint16_t *probs, unsigned bits) {
    unsigned m = 1;
    for (unsigned i = 0; i < bits; i++) m = (m << 1) | rc_bit(rc, probs + m);
    return m - (1u << bits);
}

INLINE unsigned tree_reverse(RangeDecoder *rc, uint16_t *probs, unsigned bits) {
    unsigned m = 1, symbol = 0;
    for (unsigned i = 0; i < bits; i++) {
        unsigned bit = rc_bit(rc, probs + m);
        m = (m << 1) | bit;
        symbol |= bit << i;
    }
    return symbol;
}

INLINE unsigned length(RangeDecoder *rc, LenProbs *p, unsigned pos_state) {
    if (!rc_bit(rc, &p->choice)) return tree(rc, p->low[pos_state], 3);
    if (!rc_bit(rc, &p->choice2)) return 8 + tree(rc, p->mid[pos_state], 3);
    return 16 + tree(rc, p->high, 8);
}

/* Returns 0 after writing exactly out_size bytes, nonzero on corrupt input. */
int boot_lzma(const uint8_t *in, uint32_t in_size, uint8_t *out, uint32_t out_size,
              BootLzmaProbs *p) {
    uint16_t *all = (uint16_t *)p;
    for (unsigned i = 0; i < sizeof *p / sizeof *all; i++) all[i] = PROB_INIT;
    RangeDecoder rc = {in, in + in_size, 0xFFFFFFFFu, 0, 0};
    if (in_size < 5 || in[0]) return 1;
    for (unsigned i = 1; i < 5; i++) rc.code = (rc.code << 8) | in[i];
    rc.in += 5;
    uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0, pos = 0;
    unsigned state = 0;
    while (pos < out_size) {
        if (rc.error) return 2;
        unsigned pos_state = pos & 3;
        if (!rc_bit(&rc, &p->is_match[state][pos_state])) {
            uint16_t *probs = p->literal[pos ? out[pos - 1] >> 5 : 0];
            unsigned symbol = 1;
            if (state >= 7) {
                if (rep0 >= pos) return 3;
                unsigned match = out[pos - rep0 - 1];
                do {
                    unsigned match_bit = (match >> 7) & 1;
                    match <<= 1;
                    unsigned bit = rc_bit(&rc, probs + ((1 + match_bit) << 8) + symbol);
                    symbol = (symbol << 1) | bit;
                    if (match_bit != bit) break;
                } while (symbol < 0x100);
            }
            while (symbol < 0x100) symbol = (symbol << 1) | rc_bit(&rc, probs + symbol);
            out[pos++] = (uint8_t)symbol;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }
        unsigned len;
        if (rc_bit(&rc, &p->is_rep[state])) {
            if (!pos) return 4;
            if (!rc_bit(&rc, &p->rep_g0[state])) {
                if (!rc_bit(&rc, &p->rep0_long[state][pos_state])) {
                    state = state < 7 ? 9 : 11;
                    if (rep0 >= pos) return 5;
                    out[pos] = out[pos - rep0 - 1];
                    pos++;
                    continue;
                }
            } else {
                uint32_t distance;
                if (!rc_bit(&rc, &p->rep_g1[state])) distance = rep1;
                else {
                    if (!rc_bit(&rc, &p->rep_g2[state])) distance = rep2;
                    else { distance = rep3; rep3 = rep2; }
                    rep2 = rep1;
                }
                rep1 = rep0;
                rep0 = distance;
            }
            len = length(&rc, &p->rep_len, pos_state);
            state = state < 7 ? 8 : 11;
        } else {
            rep3 = rep2; rep2 = rep1; rep1 = rep0;
            len = length(&rc, &p->len, pos_state);
            state = state < 7 ? 7 : 10;
            unsigned slot = tree(&rc, p->slot[len < 3 ? len : 3], 6);
            if (slot < 4) rep0 = slot;
            else {
                unsigned bits = (slot >> 1) - 1;
                rep0 = (2 | (slot & 1)) << bits;
                if (slot < 14) rep0 += tree_reverse(&rc, p->special + rep0 - slot, bits);
                else {
                    rep0 += rc_direct(&rc, bits - 4) << 4;
                    rep0 += tree_reverse(&rc, p->align, 4);
                }
            }
            if (rep0 == 0xFFFFFFFFu) return 6; /* End marker before out_size. */
        }
        len += 2;
        if (rep0 >= pos || len > out_size - pos) return 7;
        while (len--) { out[pos] = out[pos - rep0 - 1]; pos++; }
    }
    return rc.error ? 8 : 0;
}
