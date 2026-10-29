// SPDX-License-Identifier: MIT
// CLI: qvcaf-cli input.png mask.png out.png
#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"

#include "qvcaf.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static void usage(const char *argv0)
{
    std::fprintf(stderr,
                 "Usage: %s <input.png> <mask.png> <out.png>\n"
                 "  mask: nonzero (or near-white) = hole to fill\n",
                 argv0);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        usage(argv[0]);
        return 2;
    }
    const char *inPath = argv[1];
    const char *maskPath = argv[2];
    const char *outPath = argv[3];

    int w = 0, h = 0, comp = 0;
    unsigned char *img = stbi_load(inPath, &w, &h, &comp, 4);
    if (!img) {
        std::fprintf(stderr, "Failed to load image: %s (%s)\n", inPath, stbi_failure_reason());
        return 1;
    }
    int mw = 0, mh = 0, mc = 0;
    unsigned char *maskImg = stbi_load(maskPath, &mw, &mh, &mc, 1);
    if (!maskImg) {
        std::fprintf(stderr, "Failed to load mask: %s (%s)\n", maskPath, stbi_failure_reason());
        stbi_image_free(img);
        return 1;
    }
    if (mw != w || mh != h) {
        std::fprintf(stderr, "Size mismatch: image %dx%d vs mask %dx%d\n", w, h, mw, mh);
        stbi_image_free(img);
        stbi_image_free(maskImg);
        return 1;
    }

    std::vector<uint8_t> mask(w * h);
    int holeCount = 0;
    for (int i = 0; i < w * h; ++i) {
        // Treat mid-gray+ as hole (handles white/binary masks).
        mask[i] = maskImg[i] > 16 ? 1 : 0;
        if (mask[i])
            ++holeCount;
    }
    stbi_image_free(maskImg);

    std::fprintf(stderr, "image=%dx%d holes=%d pixels=%d\n", w, h, holeCount, w * h);
    if (holeCount > qvcaf::kConfirmHole)
        std::fprintf(stderr, "warning: holeCount %d > %d (UI would confirm)\n", holeCount, qvcaf::kConfirmHole);
    if (holeCount > qvcaf::kHugeHole || w * h > qvcaf::kHugePixels)
        std::fprintf(stderr, "note: huge hole/image — will downsample fill then upsample\n");

    qvcaf::FillOptions opts;
    opts.onProgress = [](int p) {
        static int last = -1;
        if (p != last && (p % 10 == 0 || p == 100)) {
            std::fprintf(stderr, "progress %d%%\n", p);
            last = p;
        }
    };

    std::vector<uint8_t> out;
    const auto t0 = std::chrono::steady_clock::now();
    const qvcaf::FillError err = qvcaf::fill(img, mask.data(), w, h, out, opts);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    stbi_image_free(img);

    if (err != qvcaf::FillError::Ok) {
        std::fprintf(stderr, "fill failed: %s (%.1f ms)\n", qvcaf::errorString(err), ms);
        return 1;
    }

    if (!stbi_write_png(outPath, w, h, 4, out.data(), w * 4)) {
        std::fprintf(stderr, "Failed to write %s\n", outPath);
        return 1;
    }
    std::fprintf(stderr, "wrote %s in %.1f ms\n", outPath, ms);
    // Machine-readable summary line for RESULTS.md
    std::printf("OK holes=%d ms=%.1f out=%s\n", holeCount, ms, outPath);
    return 0;
}
