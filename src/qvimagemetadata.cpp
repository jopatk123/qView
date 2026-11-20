#include "qvimagemetadata.h"

#ifdef COCOA_LOADED
#include "qvcocoafunctions.h"
#endif

#include <QDateTime>
#include <QFile>
#include <QLocale>
#include <QtEndian>
#include <QVector>

#include <cstring>

namespace {

constexpr qint64 kMaxReadBytes = 8 * 1024 * 1024;

QString formatExposureTime(double seconds)
{
    if (seconds <= 0)
        return {};
    if (seconds >= 1.0)
        return QString::number(seconds, 'f', seconds >= 10.0 ? 0 : 1) + QLatin1Char('s');
    const int denom = qMax(1, qRound(1.0 / seconds));
    return QStringLiteral("1/%1s").arg(denom);
}

QString formatExifDate(const QString &raw)
{
    QDateTime dateTime = QDateTime::fromString(raw, QStringLiteral("yyyy:MM:dd HH:mm:ss"));
    if (!dateTime.isValid())
        dateTime = QDateTime::fromString(raw, Qt::ISODate);
    if (!dateTime.isValid())
        return raw;
    return QLocale::system().toString(dateTime, QLocale::ShortFormat);
}

class TiffView
{
public:
    TiffView(const uchar *data, int size, int tiffOffset, bool littleEndian)
        : m_data(data), m_size(size), m_base(tiffOffset), m_le(littleEndian)
    {
    }

    bool valid() const { return m_base >= 0 && m_base + 8 <= m_size; }

    quint16 u16(int absOffset) const
    {
        if (absOffset < 0 || absOffset + 2 > m_size)
            return 0;
        const uchar *p = m_data + absOffset;
        return m_le ? qFromLittleEndian<quint16>(p) : qFromBigEndian<quint16>(p);
    }

    quint32 u32(int absOffset) const
    {
        if (absOffset < 0 || absOffset + 4 > m_size)
            return 0;
        const uchar *p = m_data + absOffset;
        return m_le ? qFromLittleEndian<quint32>(p) : qFromBigEndian<quint32>(p);
    }

    qint32 i32(int absOffset) const { return static_cast<qint32>(u32(absOffset)); }

    bool contains(int absOffset, int bytes) const
    {
        return absOffset >= 0 && bytes >= 0 && absOffset + bytes <= m_size;
    }

    int fileOffset(quint32 tiffRel) const
    {
        if (tiffRel > static_cast<quint32>(m_size))
            return -1;
        const qint64 abs = static_cast<qint64>(m_base) + tiffRel;
        if (abs < 0 || abs > m_size)
            return -1;
        return static_cast<int>(abs);
    }

    QByteArray bytes(int absOffset, int length) const
    {
        if (!contains(absOffset, length))
            return {};
        return QByteArray(reinterpret_cast<const char *>(m_data + absOffset), length);
    }

    quint32 firstIfdOffset() const { return u32(m_base + 4); }

private:
    const uchar *m_data;
    int m_size;
    int m_base;
    bool m_le;
};

int typeSize(quint16 type)
{
    switch (type) {
    case 1:
    case 2:
    case 6:
    case 7:
        return 1;
    case 3:
    case 8:
        return 2;
    case 4:
    case 9:
    case 11:
        return 4;
    case 5:
    case 10:
    case 12:
        return 8;
    default:
        return 0;
    }
}

int valueOffset(const TiffView &tiff, int entryAbs, quint16 type, quint32 count)
{
    const int unit = typeSize(type);
    if (unit <= 0)
        return -1;
    const qint64 bytes = static_cast<qint64>(unit) * count;
    if (bytes <= 4)
        return entryAbs + 8;
    return tiff.fileOffset(tiff.u32(entryAbs + 8));
}

QString readAscii(const TiffView &tiff, int entryAbs, quint16 type, quint32 count)
{
    if (type != 2 || count == 0 || count > 4096)
        return {};
    const int offset = valueOffset(tiff, entryAbs, type, count);
    if (offset < 0)
        return {};
    QByteArray raw = tiff.bytes(offset, static_cast<int>(count));
    while (raw.endsWith('\0'))
        raw.chop(1);
    return QString::fromLatin1(raw).trimmed();
}

bool readDouble(const TiffView &tiff, int entryAbs, quint16 type, quint32 count, double *out)
{
    const int offset = valueOffset(tiff, entryAbs, type, count);
    if (offset < 0 || count == 0)
        return false;

    auto rational = [&](int index, bool signedRational) -> double {
        const int pos = offset + index * 8;
        if (!tiff.contains(pos, 8))
            return 0;
        const quint32 num = tiff.u32(pos);
        const quint32 den = tiff.u32(pos + 4);
        if (den == 0)
            return 0;
        if (!signedRational)
            return static_cast<double>(num) / static_cast<double>(den);
        return static_cast<double>(tiff.i32(pos)) / static_cast<double>(tiff.i32(pos + 4));
    };

    switch (type) {
    case 1:
        if (!tiff.contains(offset, 1))
            return false;
        *out = tiff.bytes(offset, 1).at(0);
        return true;
    case 3:
        *out = tiff.u16(offset);
        return tiff.contains(offset, 2);
    case 4:
        *out = tiff.u32(offset);
        return tiff.contains(offset, 4);
    case 5:
        if (count >= 3) {
            *out = rational(0, false) + rational(1, false) / 60.0 + rational(2, false) / 3600.0;
            return true;
        }
        *out = rational(0, false);
        return true;
    case 9:
        *out = tiff.i32(offset);
        return tiff.contains(offset, 4);
    case 10:
        *out = rational(0, true);
        return true;
    default:
        return false;
    }
}

struct ParsedExif
{
    QString takenRaw;
    QString tiffDate;
    QString make;
    QString model;
    QString lens;
    double exposureTime = 0;
    bool hasExposureTime = false;
    double fNumber = 0;
    bool hasFNumber = false;
    double iso = 0;
    bool hasIso = false;
    double focalLength = 0;
    bool hasFocalLength = false;
    double latitude = 0;
    double longitude = 0;
    bool hasLatitude = false;
    bool hasLongitude = false;
    QString latRef;
    QString lonRef;
    double altitude = 0;
    bool hasAltitude = false;
    quint8 altitudeRef = 0;
};

enum class IfdKind { Main, Gps };

void parseIfd(const TiffView &tiff, quint32 tiffRel, ParsedExif *out, int depth, IfdKind kind)
{
    if (depth > 6 || tiffRel == 0)
        return;
    const int ifdAbs = tiff.fileOffset(tiffRel);
    if (ifdAbs < 0 || !tiff.contains(ifdAbs, 2))
        return;
    const quint16 count = tiff.u16(ifdAbs);
    if (count == 0 || count > 256)
        return;
    if (!tiff.contains(ifdAbs, 2 + count * 12))
        return;

    quint32 exifIfd = 0;
    quint32 gpsIfd = 0;

    for (quint16 i = 0; i < count; ++i) {
        const int entry = ifdAbs + 2 + i * 12;
        const quint16 tag = tiff.u16(entry);
        const quint16 type = tiff.u16(entry + 2);
        const quint32 valueCount = tiff.u32(entry + 4);
        double number = 0;

        if (kind == IfdKind::Gps) {
            switch (tag) {
            case 0x0001:
                out->latRef = readAscii(tiff, entry, type, valueCount).toUpper();
                break;
            case 0x0002:
                if (readDouble(tiff, entry, type, valueCount, &number)) {
                    out->latitude = number;
                    out->hasLatitude = true;
                }
                break;
            case 0x0003:
                out->lonRef = readAscii(tiff, entry, type, valueCount).toUpper();
                break;
            case 0x0004:
                if (readDouble(tiff, entry, type, valueCount, &number)) {
                    out->longitude = number;
                    out->hasLongitude = true;
                }
                break;
            case 0x0005:
                if (readDouble(tiff, entry, type, valueCount, &number))
                    out->altitudeRef = static_cast<quint8>(number);
                break;
            case 0x0006:
                if (readDouble(tiff, entry, type, valueCount, &number)) {
                    out->altitude = number;
                    out->hasAltitude = true;
                }
                break;
            default:
                break;
            }
            continue;
        }

        switch (tag) {
        case 0x010F:
            out->make = readAscii(tiff, entry, type, valueCount);
            break;
        case 0x0110:
            out->model = readAscii(tiff, entry, type, valueCount);
            break;
        case 0x0132:
            out->tiffDate = readAscii(tiff, entry, type, valueCount);
            break;
        case 0x829A:
            if (readDouble(tiff, entry, type, valueCount, &number)) {
                out->exposureTime = number;
                out->hasExposureTime = true;
            }
            break;
        case 0x829D:
            if (readDouble(tiff, entry, type, valueCount, &number)) {
                out->fNumber = number;
                out->hasFNumber = true;
            }
            break;
        case 0x8825:
            gpsIfd = tiff.u32(entry + 8);
            break;
        case 0x8769:
            exifIfd = tiff.u32(entry + 8);
            break;
        case 0x8827:
            if (readDouble(tiff, entry, type, valueCount, &number)) {
                out->iso = number;
                out->hasIso = true;
            }
            break;
        case 0x9003:
            out->takenRaw = readAscii(tiff, entry, type, valueCount);
            break;
        case 0x920A:
            if (readDouble(tiff, entry, type, valueCount, &number)) {
                out->focalLength = number;
                out->hasFocalLength = true;
            }
            break;
        case 0xA434:
            out->lens = readAscii(tiff, entry, type, valueCount);
            break;
        default:
            break;
        }
    }

    parseIfd(tiff, exifIfd, out, depth + 1, IfdKind::Main);
    parseIfd(tiff, gpsIfd, out, depth + 1, IfdKind::Gps);
}

bool looksLikeTiff(const uchar *data, int size, int offset)
{
    if (offset < 0 || offset + 8 > size)
        return false;
    const uchar *p = data + offset;
    return (p[0] == 'I' && p[1] == 'I' && p[2] == 42 && p[3] == 0)
            || (p[0] == 'M' && p[1] == 'M' && p[2] == 0 && p[3] == 42);
}

int tiffStartAfterExifHeader(const uchar *data, int size, int exifMarker)
{
    const int afterHeader = exifMarker + 6;
    if (looksLikeTiff(data, size, afterHeader))
        return afterHeader;
    for (int i = 0; i < 16; ++i) {
        if (looksLikeTiff(data, size, afterHeader + i))
            return afterHeader + i;
    }
    return -1;
}

ParsedExif parseTiff(const uchar *data, int size, int tiffOffset)
{
    ParsedExif parsed;
    if (!looksLikeTiff(data, size, tiffOffset))
        return parsed;
    const bool little = data[tiffOffset] == 'I';
    TiffView tiff(data, size, tiffOffset, little);
    parseIfd(tiff, tiff.firstIfdOffset(), &parsed, 0, IfdKind::Main);
    return parsed;
}

int findJpegExif(const uchar *data, int size)
{
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8)
        return -1;
    int offset = 2;
    while (offset + 4 <= size) {
        if (data[offset] != 0xFF)
            return -1;
        while (offset < size && data[offset] == 0xFF)
            ++offset;
        if (offset >= size)
            return -1;
        const uchar marker = data[offset++];
        if (marker == 0xD9 || marker == 0xDA)
            return -1;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
            continue;
        if (offset + 2 > size)
            return -1;
        const int length = qFromBigEndian<quint16>(data + offset);
        if (length < 2 || offset + length > size)
            return -1;
        if (marker == 0xE1 && length >= 8) {
            const int payload = offset + 2;
            if (payload + 6 <= size && memcmp(data + payload, "Exif\0\0", 6) == 0)
                return payload;
        }
        offset += length;
    }
    return -1;
}

int findPngExif(const uchar *data, int size)
{
    static const uchar signature[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (size < 8 || memcmp(data, signature, 8) != 0)
        return -1;
    int offset = 8;
    while (offset + 12 <= size) {
        const quint32 length = qFromBigEndian<quint32>(data + offset);
        if (offset + 12 + static_cast<int>(length) > size)
            return -1;
        if (memcmp(data + offset + 4, "eXIf", 4) == 0)
            return offset + 8;
        offset += 12 + static_cast<int>(length);
    }
    return -1;
}

int findWebpExif(const uchar *data, int size)
{
    if (size < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WEBP", 4) != 0)
        return -1;
    int offset = 12;
    while (offset + 8 <= size) {
        const quint32 length = qFromLittleEndian<quint32>(data + offset + 4);
        const int payload = offset + 8;
        if (payload + static_cast<int>(length) > size)
            return -1;
        if (memcmp(data + offset, "EXIF", 4) == 0)
            return payload;
        offset = payload + static_cast<int>(length);
        if (length & 1)
            ++offset;
    }
    return -1;
}

qint64 boxSize(const uchar *data, int size, int offset, int *headerSize)
{
    if (offset + 8 > size)
        return -1;
    quint32 size32 = qFromBigEndian<quint32>(data + offset);
    if (size32 == 1) {
        if (offset + 16 > size)
            return -1;
        *headerSize = 16;
        return static_cast<qint64>(qFromBigEndian<quint64>(data + offset + 8));
    }
    *headerSize = 8;
    if (size32 == 0)
        return size - offset;
    return size32;
}

int findIsobmffExif(const uchar *data, int size)
{
    if (size < 12 || memcmp(data + 4, "ftyp", 4) != 0)
        return -1;

    struct Range
    {
        int start;
        int end;
    };
    QVector<Range> stack;
    stack.append({ 0, size });
    while (!stack.isEmpty()) {
        const Range range = stack.takeLast();
        int offset = range.start;
        while (offset + 8 <= range.end) {
            int headerSize = 8;
            const qint64 total = boxSize(data, size, offset, &headerSize);
            if (total < headerSize)
                break;
            const int boxEnd = offset
                    + static_cast<int>(qMin(total, static_cast<qint64>(range.end - offset)));
            const char *type = reinterpret_cast<const char *>(data + offset + 4);
            const int payload = offset + headerSize;
            if (memcmp(type, "Exif", 4) == 0)
                return payload;
            if (memcmp(type, "meta", 4) == 0 || memcmp(type, "moov", 4) == 0
                || memcmp(type, "iprp", 4) == 0 || memcmp(type, "ipco", 4) == 0) {
                int child = payload;
                if (memcmp(type, "meta", 4) == 0)
                    child += 4;
                if (child < boxEnd)
                    stack.append({ child, boxEnd });
            }
            if (boxEnd <= offset)
                break;
            offset = boxEnd;
        }
    }
    return -1;
}

int findExifMagic(const uchar *data, int size)
{
    for (int i = 0; i + 10 <= size; ++i) {
        if (memcmp(data + i, "Exif\0\0", 6) == 0 && looksLikeTiff(data, size, i + 6))
            return i;
    }
    return -1;
}

int locateTiff(const uchar *data, int size)
{
    if (looksLikeTiff(data, size, 0))
        return 0;

    auto resolve = [&](int payload) -> int {
        if (payload < 0)
            return -1;
        if (looksLikeTiff(data, size, payload))
            return payload;
        return tiffStartAfterExifHeader(data, size, payload);
    };

    const int jpeg = findJpegExif(data, size);
    if (jpeg >= 0)
        return resolve(jpeg);

    const int png = findPngExif(data, size);
    if (png >= 0)
        return resolve(png);

    const int webp = findWebpExif(data, size);
    if (webp >= 0)
        return resolve(webp);

    const int heif = findIsobmffExif(data, size);
    if (heif >= 0)
        return resolve(heif);

    const int magic = findExifMagic(data, size);
    if (magic >= 0)
        return magic + 6;

    return -1;
}

QVImageMetadata::Fields toFields(const ParsedExif &parsed)
{
    QVImageMetadata::Fields fields;
    if (!parsed.takenRaw.isEmpty())
        fields.taken = formatExifDate(parsed.takenRaw);
    else if (!parsed.tiffDate.isEmpty())
        fields.taken = formatExifDate(parsed.tiffDate);

    if (!parsed.make.isEmpty() && !parsed.model.isEmpty()) {
        fields.camera = parsed.model.startsWith(parsed.make, Qt::CaseInsensitive)
                ? parsed.model
                : (parsed.make + QLatin1Char(' ') + parsed.model);
    } else {
        fields.camera = !parsed.model.isEmpty() ? parsed.model : parsed.make;
    }
    if (!parsed.lens.isEmpty() && !fields.camera.contains(parsed.lens)) {
        if (fields.camera.isEmpty())
            fields.camera = parsed.lens;
        else
            fields.camera += QStringLiteral(" · ") + parsed.lens;
    }

    QStringList exposureParts;
    if (parsed.hasExposureTime) {
        const QString exposure = formatExposureTime(parsed.exposureTime);
        if (!exposure.isEmpty())
            exposureParts.append(exposure);
    }
    if (parsed.hasFNumber && parsed.fNumber > 0)
        exposureParts.append(QStringLiteral("f/%1").arg(QString::number(parsed.fNumber, 'g', 3)));
    if (parsed.hasIso && parsed.iso > 0)
        exposureParts.append(QStringLiteral("ISO %1").arg(qRound(parsed.iso)));
    if (parsed.hasFocalLength && parsed.focalLength > 0)
        exposureParts.append(
                QStringLiteral("%1 mm").arg(QString::number(parsed.focalLength, 'g', 4)));
    fields.exposure = exposureParts.join(QStringLiteral(" · "));

    if (parsed.hasLatitude && parsed.hasLongitude) {
        double latitude = parsed.latitude;
        double longitude = parsed.longitude;
        if (parsed.latRef == QLatin1String("S"))
            latitude = -qAbs(latitude);
        else if (parsed.latRef == QLatin1String("N"))
            latitude = qAbs(latitude);
        if (parsed.lonRef == QLatin1String("W"))
            longitude = -qAbs(longitude);
        else if (parsed.lonRef == QLatin1String("E"))
            longitude = qAbs(longitude);

        const QChar latHemisphere = latitude >= 0 ? QLatin1Char('N') : QLatin1Char('S');
        const QChar lonHemisphere = longitude >= 0 ? QLatin1Char('E') : QLatin1Char('W');
        fields.gps = QStringLiteral("%1° %2, %3° %4")
                             .arg(qAbs(latitude), 0, 'f', 6)
                             .arg(latHemisphere)
                             .arg(qAbs(longitude), 0, 'f', 6)
                             .arg(lonHemisphere);
        if (parsed.hasAltitude) {
            const double signedAltitude =
                    parsed.altitudeRef == 1 ? -parsed.altitude : parsed.altitude;
            fields.gps += QStringLiteral(" · %1 m").arg(QString::number(signedAltitude, 'f', 1));
        }
    }
    return fields;
}

QVImageMetadata::Fields parseFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray data = file.read(kMaxReadBytes);
    if (data.size() < 12)
        return {};
    const uchar *bytes = reinterpret_cast<const uchar *>(data.constData());
    const int tiffOffset = locateTiff(bytes, data.size());
    if (tiffOffset < 0)
        return {};
    return toFields(parseTiff(bytes, data.size(), tiffOffset));
}

bool hasFields(const QVImageMetadata::Fields &fields)
{
    return !fields.taken.isEmpty() || !fields.camera.isEmpty() || !fields.exposure.isEmpty()
            || !fields.gps.isEmpty();
}

} // namespace

QVImageMetadata::Fields QVImageMetadata::read(const QString &filePath)
{
    if (filePath.isEmpty())
        return {};
#ifdef COCOA_LOADED
    const QVCocoaFunctions::ImageMetadata cocoa = QVCocoaFunctions::imageMetadata(filePath);
    Fields fields;
    fields.taken = cocoa.taken;
    fields.camera = cocoa.camera;
    fields.exposure = cocoa.exposure;
    fields.gps = cocoa.gps;
    if (hasFields(fields))
        return fields;
#endif
    return parseFile(filePath);
}
