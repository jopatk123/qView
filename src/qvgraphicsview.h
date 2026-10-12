#ifndef QVGRAPHICSVIEW_H
#define QVGRAPHICSVIEW_H

#include "qvimagecore.h"
#include <QGraphicsView>
#include <QImageReader>
#include <QMimeData>
#include <QDir>
#include <QTimer>
#include <QFileInfo>
#include <QRubberBand>

class QVGraphicsView : public QGraphicsView
{
    Q_OBJECT

public:
    QVGraphicsView(QWidget *parent = nullptr);

    enum class ScaleMode { resetScale, zoom };
    Q_ENUM(ScaleMode)

    enum class GoToFileMode { constant, first, previous, next, last };
    Q_ENUM(GoToFileMode)

    QMimeData *getMimeData() const;
    void loadMimeData(const QMimeData *mimeData);
    void loadFile(const QString &fileName);

    void reloadFile();

    void zoomIn(const QPoint &pos = QPoint(-1, -1));

    void zoomOut(const QPoint &pos = QPoint(-1, -1));

    void zoom(qreal scaleFactor, const QPoint &pos = QPoint(-1, -1));

    void scaleExpensively();
    void makeUnscaled();

    void resetScale();
    void originalSize();

    void goToFile(const GoToFileMode &mode, int index = 0);

    void settingsUpdated();

    void closeImage();
    void jumpToNextFrame();
    void setPaused(const bool &desiredState);
    void setSpeed(const int &desiredSpeed);
    void rotateImage(int rotation);
    void mirrorImage();
    void flipImage();
    bool beginCrop();
    void cancelCrop();
    bool isCropping() const { return cropMode; }
    bool compressImage(const QSize &targetSize, int quality);
    bool applyCompression(const QVImageCore::CompressionOutput &output, int quality);
    QByteArray originalFormat() const { return imageCore.originalFormat(); }
    qint64 pendingEncodedBytes() const { return imageCore.pendingEncodedBytes(); }
    QString saveOverOriginal();
    void revertEdits();
    bool isImageModified() const { return imageCore.isImageModified(); }
    bool canEditStill() const { return imageCore.canEditStill(); }

    const QVImageCore::FileDetails &getCurrentFileDetails() const
    {
        return imageCore.getCurrentFileDetails();
    }
    const QPixmap &getLoadedPixmap() const { return imageCore.getLoadedPixmap(); }
    const QMovie &getLoadedMovie() const { return imageCore.getLoadedMovie(); }

signals:
    void cancelSlideshow();

    void fileChanged();

    void updatedLoadedPixmapItem();

    void imageStateChanged();

    void cropModeChanged(bool active);

protected:
    void wheelEvent(QWheelEvent *event) override;

    void resizeEvent(QResizeEvent *event) override;

    void dropEvent(QDropEvent *event) override;

    void dragEnterEvent(QDragEnterEvent *event) override;

    void dragMoveEvent(QDragMoveEvent *event) override;

    void dragLeaveEvent(QDragLeaveEvent *event) override;

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    void enterEvent(QEvent *event) override;
#else
    void enterEvent(QEnterEvent *event) override;
#endif

    void mousePressEvent(QMouseEvent *event) override;

    void mouseMoveEvent(QMouseEvent *event) override;

    void mouseReleaseEvent(QMouseEvent *event) override;

    bool event(QEvent *event) override;

    void fitInViewMarginless(const QRectF &rect);
    void fitInViewMarginless(const QGraphicsItem *item);

    void centerOn(const QPointF &pos);

    void centerOn(qreal x, qreal y);

    void centerOn(const QGraphicsItem *item);

private slots:
    void animatedFrameChanged(QRect rect);

    void postLoad();

    void updateLoadedPixmapItem();

private:
    void updateFilteringMode();
    QRect pixmapRectForViewportRect(const QRect &viewportRect) const;

    QGraphicsPixmapItem *loadedPixmapItem;

    constexpr static int MARGIN = -2;
    constexpr static qreal MAX_EXPENSIVE_SCALING_SIZE = 3;

    // Set to too high a value to activate for now...
    constexpr static qreal MAX_FILTERING_SIZE = 5000;

    qreal currentScale;
    QSize scaledSize;
    bool isOriginalSize;
    QPoint lastZoomEventPos;
    QPointF lastZoomRoundingError;
    QPointF lastScrollRoundingError;

    QTransform absoluteTransform;
    QTransform zoomBasis;
    qreal zoomBasisScaleFactor;

    QVImageCore imageCore{ this };

    QTimer *expensiveScaleTimerNew;
    QPointF centerPoint;
    Qt::MouseButton mousePressButton;
    Qt::KeyboardModifiers mousePressModifiers;
    QPoint mousePressPosition;

    bool cropMode = false;
    bool cropDragging = false;
    QPoint cropOrigin;
    QRubberBand *cropRubberBand = nullptr;
};
#endif // QVGRAPHICSVIEW_H
