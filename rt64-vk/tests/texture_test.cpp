/*
 * texture_test — RT64_CreateTexture through the C ABI, headless.
 *
 * Checks mip chain length, non-power-of-two and non-square handling, explicit
 * row pitch, and that malformed input is rejected rather than reading past the
 * caller's buffer.
 */
#include "rt64/rt64.h"
#include "rt64_texture_vk.h"

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
    const char *RT64_GetLastError(void);
    RT64_DEVICE *RT64_CreateDevice(void *hwnd);
    void RT64_DestroyDevice(RT64_DEVICE *device);
    RT64_TEXTURE *RT64_CreateTexture(RT64_DEVICE *device, RT64_TEXTURE_DESC desc);
    void RT64_DestroyTexture(RT64_TEXTURE *texture);
}

namespace {

int failures = 0;
void expect(bool cond, const char *what) {
    std::printf("   %s %s\n", cond ? "\033[32m ok \033[0m" : "\033[31mFAIL\033[0m", what);
    if (!cond) failures++;
}

RT64_TEXTURE_DESC makeDesc(std::vector<unsigned char> &pixels, int w, int h,
                           int format, int rowPitch = 0) {
    pixels.assign((size_t)w * h * 4, 0xA0);
    RT64_TEXTURE_DESC desc = {};
    desc.bytes = pixels.data();
    desc.byteCount = (int)pixels.size();
    desc.width = w;
    desc.height = h;
    desc.rowPitch = rowPitch;
    desc.format = format;
    return desc;
}

/* ceil(log2(max(w,h))) + 1 */
uint32_t expectedMips(int w, int h) {
    uint32_t largest = (uint32_t)(w > h ? w : h);
    uint32_t levels = 1;
    while (largest > 1) { largest >>= 1; levels++; }
    return levels;
}

} /* namespace */

int main() {
    RT64_DEVICE *device = RT64_CreateDevice(nullptr);
    if (device == nullptr) {
        std::fprintf(stderr, "RT64_CreateDevice failed: %s\n", RT64_GetLastError());
        return 1;
    }
    std::printf("  device created\n");

    struct Case { int w, h; const char *label; };
    const Case cases[] = {
        { 256, 256, "256x256 power of two" },
        {  64, 128, "64x128 non-square" },
        {  96,  48, "96x48 non-power-of-two" },
        {   1,   1, "1x1 minimum" },
    };

    for (const Case &c : cases) {
        std::vector<unsigned char> pixels;
        RT64_TEXTURE_DESC desc = makeDesc(pixels, c.w, c.h,
                                          RT64_TEXTURE_FORMAT_RGBA8);
        RT64_TEXTURE *tex = RT64_CreateTexture(device, desc);
        if (tex == nullptr) {
            std::printf("   \033[31mFAIL\033[0m %s: %s\n", c.label,
                        RT64_GetLastError());
            failures++;
            continue;
        }
        RT64::TextureVK *t = (RT64::TextureVK *)tex;
        const uint32_t want = expectedMips(c.w, c.h);
        std::printf("   %s %s -> %u mip levels (expected %u)\n",
                    t->getMipLevels() == want ? "\033[32m ok \033[0m"
                                              : "\033[31mFAIL\033[0m",
                    c.label, t->getMipLevels(), want);
        if (t->getMipLevels() != want) failures++;
        expect(t->getView() != VK_NULL_HANDLE, "  image view created");
        RT64_DestroyTexture(tex);
    }

    /* Explicit row pitch with padding at the end of each row. */
    {
        const int w = 60, h = 32, pitch = 256;   /* 60*4 = 240, padded to 256 */
        std::vector<unsigned char> pixels((size_t)pitch * h, 0x7F);
        RT64_TEXTURE_DESC desc = {};
        desc.bytes = pixels.data();
        desc.byteCount = (int)pixels.size();
        desc.width = w;
        desc.height = h;
        desc.rowPitch = pitch;
        desc.format = RT64_TEXTURE_FORMAT_RGBA8;
        RT64_TEXTURE *tex = RT64_CreateTexture(device, desc);
        expect(tex != nullptr, "padded row pitch accepted");
        if (tex) RT64_DestroyTexture(tex);
    }

    /* Rejections. */
    {
        std::vector<unsigned char> pixels;
        RT64_TEXTURE_DESC desc = makeDesc(pixels, 64, 64,
                                          RT64_TEXTURE_FORMAT_RGBA8);
        desc.byteCount = 16;   /* far too small for 64x64 */
        RT64_TEXTURE *tex = RT64_CreateTexture(device, desc);
        expect(tex == nullptr, "undersized byteCount rejected");
        expect(std::string(RT64_GetLastError()).find("smaller than") != std::string::npos,
               "  and explains why");
    }
    {
        std::vector<unsigned char> pixels;
        RT64_TEXTURE_DESC desc = makeDesc(pixels, 32, 32,
                                          RT64_TEXTURE_FORMAT_DDS);
        RT64_TEXTURE *tex = RT64_CreateTexture(device, desc);
        expect(tex == nullptr, "DDS reports unimplemented rather than crashing");
        expect(std::string(RT64_GetLastError()).find("DDS") != std::string::npos,
               "  and names the format");
    }

    RT64_DestroyDevice(device);
    std::printf("  %s\n", failures == 0 ? "all texture checks passed"
                                        : "SOME CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
