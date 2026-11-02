// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ch0ya
//
// SHA-256 (FIPS 180-4). Written for this project: the tree has no hash of its own, and the loader must check the image
// it downloads against the digest GitHub publishes for the release asset. Dependency-free on purpose (the loader is
// built for two very different systems, and the Linux ABI adapters already carry enough of the C library). The code is
// ordinary public-domain-style FIPS 180-4, so there is no third-party notice to carry.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t length;             // total bytes fed in, for the length padding
    unsigned char buffer[64];    // a partial block, carried between calls
    size_t buffered;
} Sha256;

void sha256Init(Sha256 *context);
void sha256Update(Sha256 *context, const void *data, size_t size);
void sha256Final(Sha256 *context, unsigned char digest[32]);
// The 32 bytes as 64 lowercase hex characters plus a terminator (the caller's array is 65 bytes).
void sha256Hex(const unsigned char digest[32], char out[65]);
