#ifndef QVPRINTPAGE_H
#define QVPRINTPAGE_H

#include <QImage>
#include <QPainter>
#include <QRectF>

// Paints one image centered on a white page, scaled to fit while keeping the
// aspect ratio. The single image print, the PDF export and the batch print
// dialog all go through here so every output looks the same.
inline void drawImageOnPage(QPainter &painter, const QRectF &pageRect, const QImage &image)
{
    painter.fillRect(pageRect, Qt::white);
    if (image.isNull() || pageRect.isEmpty())
        return;

    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QSizeF targetSize(image.size());
    targetSize.scale(pageRect.size(), Qt::KeepAspectRatio);

    const QRectF targetRect(pageRect.x() + (pageRect.width() - targetSize.width()) / 2.0,
                            pageRect.y() + (pageRect.height() - targetSize.height()) / 2.0,
                            targetSize.width(), targetSize.height());
    painter.drawImage(targetRect, image);
}

#endif // QVPRINTPAGE_H
