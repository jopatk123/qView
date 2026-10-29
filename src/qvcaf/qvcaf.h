// SPDX-License-Identifier: MIT
// Multi-scale PatchMatch content-aware fill (Barnes et al. family).
// Ported from open-ps caf/patchmatch-inpaint.ts for qView Phase 1.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace qvcaf {

enum class FillError {
    Ok = 0,
    Empty,       // mask has no hole pixels
    NoContext,   // not enough sampleable source patches
    Failed       // invalid args or internal failure
};

struct FillOptions {
    // Optional progress callback: 0..100
    std::function<void(int)> onProgress;

    // Quality overrides (-1 = choose from hole area; defaults bias toward quality).
    int radius = -1;
    int pmIters = -1;
    int searchCap = -1;

    // Edge fusion (quality-first defaults).
    // Width of known-neighbor feather band inside the hole (pixels).
    int featherWidth = 4;
    // Extra soft-edge band: pull hole perimeter toward known-neighbor reconstruction
    // (smooth falloff; does not reintroduce original hole content).
    int softEdgeWidth = 3;
    // Match hole-boundary mean/gain to adjacent known pixels (with distance falloff).
    bool colorMatch = true;
    // Multiplier on Jacobi harmonize iterations (1.0 = strengthened baseline).
    float harmonizeStrength = 1.25f;
};

// Fill mask pixels (nonzero = hole) from surrounding texture.
// rgba: width*height*4 bytes, RGBA order, stride = width*4
// mask: width*height bytes, nonzero = hole
// Returns filled RGBA (known pixels copied verbatim from input).
// On error, outRgba is empty and FillError != Ok.
FillError fill(const uint8_t *rgba,
               const uint8_t *mask,
               int width,
               int height,
               std::vector<uint8_t> &outRgba,
               const FillOptions &options = FillOptions());

const char *errorString(FillError err);

// Guard thresholds (match open-ps)
constexpr int kHugeHole = 280000;
constexpr int kHugePixels = 2000000;
constexpr int kConfirmHole = 120000; // orchestration-side confirm threshold

} // namespace qvcaf
