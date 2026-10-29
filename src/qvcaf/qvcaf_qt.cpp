// SPDX-License-Identifier: MIT
#include "qvcaf_qt.h"

#include <QSize>
#include <vector>

namespace qvcaf {

int countHolePixels(const QSize &imageSize, const QRect &holeRect)
{
    const QRect bounded = holeRect.intersected(QRect(QPoint(0, 0), imageSize));
    if (bounded.width() < 1 || bounded.height() < 1)
        return 0;
    return bounded.width() * bounded.height();
}

QtFillResult fillImage(const QImage &source, const QRect &holeRect, const FillOptions &options)
{
    QtFillResult result;
    if (source.isNull() || source.width() < 1 || source.height() < 1) {
        result.error = FillError::Failed;
        return result;
    }

    const QRect bounded = holeRect.intersected(source.rect());
    result.holePixels = countHolePixels(source.size(), bounded);
    if (result.holePixels <= 0) {
        result.error = FillError::Empty;
        return result;
    }

    // Prefer a packed 32-bit format so scanlines are easy to read.
    QImage src = source;
    if (src.format() != QImage::Format_ARGB32 && src.format() != QImage::Format_RGB32
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        && src.format() != QImage::Format_RGBA8888
#endif
    ) {
        src = src.convertToFormat(QImage::Format_ARGB32);
    }
    if (src.isNull()) {
        result.error = FillError::Failed;
        return result;
    }

    const int w = src.width();
    const int h = src.height();
    const int n = w * h;

    std::vector<uint8_t> rgba(static_cast<size_t>(n) * 4);
    std::vector<uint8_t> mask(static_cast<size_t>(n), 0);

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    if (src.format() == QImage::Format_RGBA8888) {
        for (int y = 0; y < h; ++y) {
            const uchar *line = src.constScanLine(y);
            uint8_t *dst = rgba.data() + static_cast<size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x) {
                dst[x * 4 + 0] = line[x * 4 + 0];
                dst[x * 4 + 1] = line[x * 4 + 1];
                dst[x * 4 + 2] = line[x * 4 + 2];
                dst[x * 4 + 3] = line[x * 4 + 3];
            }
        }
    } else
#endif
    {
        for (int y = 0; y < h; ++y) {
            const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
            uint8_t *dst = rgba.data() + static_cast<size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x) {
                const QRgb px = line[x];
                dst[x * 4 + 0] = static_cast<uint8_t>(qRed(px));
                dst[x * 4 + 1] = static_cast<uint8_t>(qGreen(px));
                dst[x * 4 + 2] = static_cast<uint8_t>(qBlue(px));
                dst[x * 4 + 3] = static_cast<uint8_t>(qAlpha(px));
            }
        }
    }

    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x)
            mask[static_cast<size_t>(y) * w + static_cast<size_t>(x)] = 1;
    }

    std::vector<uint8_t> out;
    result.error = fill(rgba.data(), mask.data(), w, h, out, options);
    if (result.error != FillError::Ok || out.size() != static_cast<size_t>(n) * 4)
        return result;

    QImage filled(w, h, QImage::Format_ARGB32);
    if (filled.isNull()) {
        result.error = FillError::Failed;
        return result;
    }
    for (int y = 0; y < h; ++y) {
        QRgb *dst = reinterpret_cast<QRgb *>(filled.scanLine(y));
        const uint8_t *srcLine = out.data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[x] = qRgba(srcLine[x * 4 + 0], srcLine[x * 4 + 1], srcLine[x * 4 + 2],
                           srcLine[x * 4 + 3]);
        }
    }

    result.image = filled;
    return result;
}

const char *friendlyErrorKey(FillError err)
{
    switch (err) {
    case FillError::Ok:
        return "ok";
    case FillError::Empty:
        return "empty";
    case FillError::NoContext:
        return "no-context";
    case FillError::Failed:
        return "failed";
    }
    return "failed";
}

} // namespace qvcaf
