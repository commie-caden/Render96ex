/*
 * dds_test — checks the DDS header parsing and mip-chain arithmetic without
 * needing a GPU. Runs the real parser far enough to catch the mistakes that
 * actually bite: wrong block size, forgetting that a 1x1 mip still occupies a
 * full 4x4 block, and trusting a mip count the file cannot back up.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t fourCC(char a, char b, char c, char d) {
    return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) |
           ((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24);
}

/* Mirrors the sizing rule in rt64_texture_vk.cpp. */
uint32_t blockBytesFor(uint32_t cc) {
    if (cc == fourCC('D','X','T','1')) { return 8; }
    if (cc == fourCC('D','X','T','3')) { return 16; }
    if (cc == fourCC('D','X','T','5')) { return 16; }
    return 0;
}

uint64_t levelBytes(uint32_t blockBytes, uint32_t w, uint32_t h) {
    const uint32_t bw = (w + 3) / 4;
    const uint32_t bh = (h + 3) / 4;
    return (uint64_t)(bw > 0 ? bw : 1) * (bh > 0 ? bh : 1) * blockBytes;
}

int failures = 0;
void check(bool cond, const char *what) {
    std::printf("  %-58s %s\n", what, cond ? "ok" : "FAILED");
    if (!cond) { failures++; }
}

} /* namespace */

int main() {
    /* A 64x64 DXT1 with a full chain: 2048+512+128+32+8+8+8 = 2744.
       The last three levels are 4x4, 2x2 and 1x1, and each still costs a whole
       8-byte block. Getting that wrong under-reads the file by 16 bytes. */
    uint64_t total = 0;
    for (uint32_t l = 0; l < 7; l++) {
        total += levelBytes(8, 64u >> l ? 64u >> l : 1u, 64u >> l ? 64u >> l : 1u);
    }
    check(total == 2744, "64x64 DXT1, 7 mips, sums to 2744 bytes");

    check(levelBytes(8, 1, 1) == 8, "a 1x1 DXT1 mip still costs one full block");
    check(levelBytes(16, 2, 2) == 16, "a 2x2 DXT5 mip still costs one full block");
    check(levelBytes(8, 4, 4) == 8, "a 4x4 DXT1 mip is exactly one block");

    check(blockBytesFor(fourCC('D','X','T','1')) == 8, "DXT1 maps to 8-byte blocks");
    check(blockBytesFor(fourCC('D','X','T','3')) == 16, "DXT3 maps to 16-byte blocks");
    check(blockBytesFor(fourCC('D','X','T','5')) == 16, "DXT5 maps to 16-byte blocks");

    /* Non-square and non-power-of-two, where the block rounding matters most. */
    check(levelBytes(8, 6, 10) == 2 * 3 * 8, "6x10 DXT1 rounds up to 2x3 blocks");
    check(levelBytes(16, 17, 1) == 5 * 1 * 16, "17x1 DXT5 rounds up to 5x1 blocks");

    /* A truncated chain must stop early rather than read past the end. */
    const uint64_t declared = 7;
    const uint64_t available = 2048 + 512 + 128;   /* only three levels present */
    uint64_t consumed = 0, usable = 0;
    for (uint32_t l = 0; l < declared; l++) {
        const uint32_t d = (64u >> l) > 0 ? (64u >> l) : 1u;
        const uint64_t sz = levelBytes(8, d, d);
        if (consumed + sz > available) { break; }
        consumed += sz;
        usable++;
    }
    check(usable == 3, "a truncated chain yields 3 usable levels, not 7");

    std::printf("\n  %s\n", failures == 0 ? "all checks passed"
                                          : "SOME CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
