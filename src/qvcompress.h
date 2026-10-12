#ifndef QVCOMPRESS_H
#define QVCOMPRESS_H

#include <QByteArray>
#include <QImage>
#include <QSize>

namespace QVCompress {

// Smooth resize. Downscales also get a light unsharp mask.
// Same-size input is returned unchanged.
QImage resize(const QImage &source, const QSize &targetSize);

// JPEG with optimized Huffman tables. Quality >= 80 keeps 4:4:4 chroma;
// lower qualities use 4:2:0. Empty when libjpeg is unavailable or encoding fails.
QByteArray encodeJpeg(const QImage &image, int quality);

} // namespace QVCompress

#endif // QVCOMPRESS_H
