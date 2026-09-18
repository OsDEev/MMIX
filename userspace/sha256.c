/*
 * sha256: SHA-256 digest of stdin (or a file argument).
 * usage: echo hello | sha256       # hashes stdin
 *        sha256 /etc/unit.conf     # hashes a file
 */
#include "libc.h"

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t H[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

static void sha_block(const uint8_t *data) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)data[i * 4] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) |
               ((uint32_t)data[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = H[0], b = H[1], c = H[2], d = H[3];
    uint32_t e = H[4], f = H[5], g = H[6], h = H[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    H[0] += a; H[1] += b; H[2] += c; H[3] += d;
    H[4] += e; H[5] += f; H[6] += g; H[7] += h;
}

/* Ring buffer of two blocks; keeps 0..127 bytes pending. */
static uint8_t pending[128];
static size_t pend = 0;

static void feed(const uint8_t *data, size_t n) {
    while (n > 0) {
        size_t take = (n < sizeof(pending) - pend) ? n : (sizeof(pending) - pend);
        memcpy(pending + pend, data, take);
        pend += take;
        data += take;
        n -= take;
        if (pend == sizeof(pending)) {
            sha_block(pending);
            sha_block(pending + 64);
            pend = 0;
        }
    }
}

static void print_hex32(uint32_t v) {
    for (int i = 28; i >= 0; i -= 4) {
        int d = (v >> i) & 0xF;
        char c = (d < 10) ? (char)('0' + d) : (char)('a' + d - 10);
        write(1, &c, 1);
    }
}

int main(int argc, char **argv) {
    int fd = 0;
    if (argc > 1) {
        fd = open(argv[1]);
        if (fd < 0) {
            print("sha256: ");
            print(argv[1]);
            print(": no such file\n");
            return 1;
        }
    }

    uint8_t buf[128];
    long total = 0;
    long r;
    while ((r = read(fd, buf, sizeof(buf))) > 0) {
        total += r;
        feed(buf, (size_t)r);
    }

    /* Final block: 0x80, zero pad to 56 mod 64, 64-bit length. */
    uint64_t bits = (uint64_t)total * 8;
    pending[pend++] = 0x80;
    while (pend % 64 != 56) {
        if (pend == sizeof(pending)) {
            sha_block(pending);
            sha_block(pending + 64);
            pend = 0;
        } else {
            pending[pend++] = 0;
        }
    }
    for (int i = 7; i >= 0; i--) {
        pending[pend++] = (uint8_t)(bits >> (i * 8));
    }
    for (size_t off = 0; off < pend; off += 64) {
        sha_block(pending + off);
    }

    for (int i = 0; i < 8; i++) print_hex32(H[i]);
    print("\n");
    return 0;
}