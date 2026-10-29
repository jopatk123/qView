// SPDX-License-Identifier: MIT
// QImage adapter for qvcaf::fill — used by the qView GUI only.
#pragma once

#include "qvcaf.h"

#include <QImage>
#include <QRect>
#include <QString>

namespace qvcaf {

struct QtFillResult {
    FillError error = FillError::Failed;
    QImage image;
    int holePixels = 0;
};

// Hole pixel count for a rectangle clipped to imageSize (for UI confirm gates).
int countHolePixels(const QSize &imageSize, const QRect &holeRect);

// Build a hole mask from holeRect (nonzero inside), run PatchMatch fill, return RGBA image.
// On error, result.image is null and error != Ok.
QtFillResult fillImage(const QImage &source, const QRect &holeRect,
                       const FillOptions &options = FillOptions());

// Localized-friendly English keys; MainWindow maps with tr().
const char *friendlyErrorKey(FillError err);

} // namespace qvcaf
