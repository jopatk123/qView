#include "qvcompress.h"

#include <QtGlobal>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <vector>

#ifdef QV_HAVE_LIBJPEG
#  include <cstdio>
#  include <csetjmp>
#  include <jpeglib.h>
#endif

namespace {

int divide256(int value)
{
    return qBound(0, (value + 128) >> 8, 255);
}

QImage gaussianBlur(const QImage &image)
{
    constexpr int kRadius = 2;
    constexpr float kSigma = 0.8f;
    float raw[kRadius * 2 + 1];
    float sum = 0.f;
    for (int i = -kRadius; i <= kRadius; ++i) {
        raw[i + kRadius] = std::exp(-0.5f * float(i * i) / (kSigma * kSigma));
        sum += raw[i + kRadius];
    }
    int kernel[kRadius * 2 + 1];
    int total = 0;
    for (int i = 0; i <= kRadius * 2; ++i) {
        kernel[i] = qRound(raw[i] / sum * 256.f);
        total += kernel[i];
    }
    kernel[kRadius] += 256 - total;

    const int width = image.width();
    const int height = image.height();
    QImage horizontal(image.size(), image.format());
    if (horizontal.isNull())
        return horizontal;

    for (int y = 0; y < height; ++y) {
        const auto *in = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        auto *out = reinterpret_cast<QRgb *>(horizontal.scanLine(y));
        for (int x = 0; x < width; ++x) {
            int blue = 0;
            int green = 0;
            int red = 0;
            int alpha = 0;
            for (int k = -kRadius; k <= kRadius; ++k) {
                const QRgb pixel = in[qBound(0, x + k, width - 1)];
                const int weight = kernel[k + kRadius];
                blue += qBlue(pixel) * weight;
                green += qGreen(pixel) * weight;
                red += qRed(pixel) * weight;
                alpha += qAlpha(pixel) * weight;
            }
            out[x] = qRgba(divide256(red), divide256(green), divide256(blue), divide256(alpha));
        }
    }

    QImage blurred(image.size(), image.format());
    if (blurred.isNull())
        return blurred;
    std::vector<const QRgb *> rows(static_cast<size_t>(height));
    for (int y = 0; y < height; ++y)
        rows[size_t(y)] = reinterpret_cast<const QRgb *>(horizontal.constScanLine(y));

    for (int y = 0; y < height; ++y) {
        auto *out = reinterpret_cast<QRgb *>(blurred.scanLine(y));
        for (int x = 0; x < width; ++x) {
            int blue = 0;
            int green = 0;
            int red = 0;
            int alpha = 0;
            for (int k = -kRadius; k <= kRadius; ++k) {
                const QRgb pixel = rows[size_t(qBound(0, y + k, height - 1))][x];
                const int weight = kernel[k + kRadius];
                blue += qBlue(pixel) * weight;
                green += qGreen(pixel) * weight;
                red += qRed(pixel) * weight;
                alpha += qAlpha(pixel) * weight;
            }
            out[x] = qRgba(divide256(red), divide256(green), divide256(blue), divide256(alpha));
        }
    }
    return blurred;
}

int sharpenChannel(int original, int blurred)
{
    return qBound(0, original + (original - blurred) * 45 / 100, 255);
}

void unsharpOpaque(QImage &image)
{
    if (image.width() < 3 || image.height() < 3)
        return;
    const QImage blurred = gaussianBlur(image);
    if (blurred.size() != image.size())
        return;

    for (int y = 0; y < image.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        const auto *soft = reinterpret_cast<const QRgb *>(blurred.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            row[x] = qRgba(sharpenChannel(qRed(row[x]), qRed(soft[x])),
                           sharpenChannel(qGreen(row[x]), qGreen(soft[x])),
                           sharpenChannel(qBlue(row[x]), qBlue(soft[x])), qAlpha(row[x]));
        }
    }
}

bool imageIsOpaque(const QImage &image)
{
    if (!image.hasAlphaChannel())
        return true;
    for (int y = 0; y < image.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(row[x]) != 255)
                return false;
        }
    }
    return true;
}

bool shrunkMeaningfully(const QSize &source, const QSize &target)
{
    return qint64(target.width()) * 50 < qint64(source.width()) * 49
            || qint64(target.height()) * 50 < qint64(source.height()) * 49;
}

#ifdef QV_HAVE_LIBJPEG

struct JpegErrorManager
{
    jpeg_error_mgr base;
    jmp_buf jump;
    unsigned char **outBuffer = nullptr;
    // Filled in the error handler before longjmp, so the buffer can be freed
    // without relying on locals that setjmp may have clobbered.
    unsigned char *volatile captured = nullptr;
};

void jpegErrorExit(j_common_ptr cinfo)
{
    auto *manager = reinterpret_cast<JpegErrorManager *>(cinfo->err);
    if (manager->outBuffer)
        manager->captured = *manager->outBuffer;
    longjmp(manager->jump, 1);
}

#endif

} // namespace

namespace QVCompress {

QImage resize(const QImage &source, const QSize &targetSize)
{
    if (source.isNull() || targetSize.isEmpty())
        return {};
    if (source.size() == targetSize)
        return source;

    QImage scaled = source.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (scaled.isNull() || !shrunkMeaningfully(source.size(), targetSize))
        return scaled;

    QImage dst = scaled;
    if (dst.format() != QImage::Format_ARGB32 && dst.format() != QImage::Format_RGB32)
        dst = dst.convertToFormat(QImage::Format_ARGB32);
    if (dst.isNull() || !imageIsOpaque(dst))
        return scaled;

    unsharpOpaque(dst);
    return dst;
}

QByteArray encodeJpeg(const QImage &image, int quality)
{
#ifndef QV_HAVE_LIBJPEG
    Q_UNUSED(image)
    Q_UNUSED(quality)
    return {};
#else
    if (image.isNull())
        return {};

    QImage src = image;
    if (src.format() != QImage::Format_ARGB32 && src.format() != QImage::Format_RGB32)
        src = src.convertToFormat(QImage::Format_ARGB32);
    if (src.isNull())
        return {};

    quality = qBound(1, quality, 100);
    QByteArray row(src.width() * 3, '\0');
    QByteArray encoded;
    unsigned char *mem = nullptr;
    unsigned long memSize = 0;

    jpeg_compress_struct cinfo;
    JpegErrorManager error;
    cinfo.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpegErrorExit;
    error.outBuffer = &mem;
    if (setjmp(error.jump)) {
        free(error.captured);
        jpeg_destroy_compress(&cinfo);
        return {};
    }

    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &mem, &memSize);
    cinfo.image_width = JDIMENSION(src.width());
    cinfo.image_height = JDIMENSION(src.height());
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    cinfo.optimize_coding = TRUE;
    cinfo.dct_method = JDCT_ISLOW;
    // 4:2:0 drops color resolution and softens colored edges. Keep 4:4:4 unless
    // the slider is deliberately turned down for a smaller file.
    const int chroma = quality >= 80 ? 1 : 2;
    cinfo.comp_info[0].h_samp_factor = chroma;
    cinfo.comp_info[0].v_samp_factor = chroma;
    cinfo.comp_info[1].h_samp_factor = 1;
    cinfo.comp_info[1].v_samp_factor = 1;
    cinfo.comp_info[2].h_samp_factor = 1;
    cinfo.comp_info[2].v_samp_factor = 1;

    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        const auto *pixels =
                reinterpret_cast<const QRgb *>(src.constScanLine(int(cinfo.next_scanline)));
        auto *out = reinterpret_cast<uchar *>(row.data());
        for (int x = 0; x < src.width(); ++x) {
            out[x * 3] = uchar(qRed(pixels[x]));
            out[x * 3 + 1] = uchar(qGreen(pixels[x]));
            out[x * 3 + 2] = uchar(qBlue(pixels[x]));
        }
        JSAMPROW scanline = out;
        jpeg_write_scanlines(&cinfo, &scanline, 1);
    }
    jpeg_finish_compress(&cinfo);

    if (mem != nullptr && memSize > 0
        && memSize <= static_cast<unsigned long>(std::numeric_limits<int>::max())) {
        encoded = QByteArray(reinterpret_cast<const char *>(mem), int(memSize));
    }
    free(mem);
    mem = nullptr;
    jpeg_destroy_compress(&cinfo);
    return encoded;
#endif
}

} // namespace QVCompress
