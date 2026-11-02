// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ch0ya
//
// SHA-256 (see sha256.h). A small FIPS 180-4 implementation: the state, a 64-byte block buffer and the standard
// transformations. No lookup of a library and no allocation, so the platform layers (whose HTTP reads come in 64 KiB
// chunks) can hash a stream as it arrives.
#include "sha256.h"
#include <string.h>

// The first 32 bits of the fractional parts of the cube roots of the first 64 primes (FIPS 180-4, section 4.2.2).
static const uint32_t ROUND_CONSTANTS[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static uint32_t rotateRight(uint32_t value, unsigned bits) { return value >> bits | value << (32 - bits); }

static void compress(Sha256 *context, const unsigned char block[64]) {
    uint32_t schedule[64];
    for (unsigned i = 0; i < 16; ++i)
        schedule[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 | (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t s0 = rotateRight(schedule[i - 15], 7) ^ rotateRight(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
        uint32_t s1 = rotateRight(schedule[i - 2], 17) ^ rotateRight(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }
    uint32_t a = context->state[0], b = context->state[1], c = context->state[2], d = context->state[3];
    uint32_t e = context->state[4], f = context->state[5], g = context->state[6], h = context->state[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp1 = h + sum1 + choice + ROUND_CONSTANTS[i] + schedule[i];
        uint32_t temp2 = sum0 + majority;
        h = g, g = f, f = e, e = d + temp1;
        d = c, c = b, b = a, a = temp1 + temp2;
    }
    context->state[0] += a, context->state[1] += b, context->state[2] += c, context->state[3] += d;
    context->state[4] += e, context->state[5] += f, context->state[6] += g, context->state[7] += h;
}

void sha256Init(Sha256 *context) {
    // The first 32 bits of the fractional parts of the square roots of the first 8 primes (FIPS 180-4, section 5.3.3).
    static const uint32_t initial[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    memcpy(context->state, initial, sizeof(initial));
    context->length = 0;
    context->buffered = 0;
}

void sha256Update(Sha256 *context, const void *data, size_t size) {
    const unsigned char *input = data;
    context->length += size;
    if (context->buffered) {
        size_t need = sizeof(context->buffer) - context->buffered;
        size_t take = size < need ? size : need;
        memcpy(context->buffer + context->buffered, input, take);
        context->buffered += take;
        input += take;
        size -= take;
        if (context->buffered == sizeof(context->buffer)) {
            compress(context, context->buffer);
            context->buffered = 0;
        }
    }
    while (size >= sizeof(context->buffer)) {
        compress(context, input);
        input += sizeof(context->buffer);
        size -= sizeof(context->buffer);
    }
    if (size) {
        memcpy(context->buffer, input, size);
        context->buffered = size;
    }
}

void sha256Final(Sha256 *context, unsigned char digest[32]) {
    uint64_t bits = context->length * 8;  // before the padding is counted in
    static const unsigned char marker = 0x80, zero = 0;
    sha256Update(context, &marker, 1);
    while (context->buffered != 56) sha256Update(context, &zero, 1);  // a whole block of padding, then room for the length
    unsigned char length[8];
    for (unsigned i = 0; i < 8; ++i) length[i] = (unsigned char)(bits >> (56 - i * 8));
    sha256Update(context, length, sizeof(length));
    for (unsigned i = 0; i < 8; ++i) {
        digest[i * 4] = (unsigned char)(context->state[i] >> 24);
        digest[i * 4 + 1] = (unsigned char)(context->state[i] >> 16);
        digest[i * 4 + 2] = (unsigned char)(context->state[i] >> 8);
        digest[i * 4 + 3] = (unsigned char)context->state[i];
    }
}

void sha256Hex(const unsigned char digest[32], char out[65]) {
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        out[i * 2] = digits[digest[i] >> 4];
        out[i * 2 + 1] = digits[digest[i] & 15];
    }
    out[64] = 0;
}
