#include "qvthumbnailbar.h"
#include "qvapplication.h"

#include <QListView>
#include <QPainter>
#include <QScrollBar>
#include <QStyle>
#include <QVBoxLayout>
#include <QResizeEvent>
#include <QWheelEvent>

// QVThumbnailModel

QVThumbnailModel::QVThumbnailModel(QObject *parent) : QAbstractListModel(parent)
{
}

int QVThumbnailModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return mFiles.size();
}

QVariant QVThumbnailModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= mFiles.size())
        return QVariant();

    const QVImageCore::CompatibleFile &file = mFiles.at(index.row());
    switch (role) {
    case FilePathRole:
        return file.absoluteFilePath;
    case CurrentRole:
        return index.row() == mCurrentRow;
    case Qt::ToolTipRole:
        return file.fileName;
    default:
        return QVariant();
    }
}

bool QVThumbnailModel::setFileList(const QList<QVImageCore::CompatibleFile> &files, int currentIndex)
{
    // If the list is identical, only the highlight has to move. This keeps the
    // scroll position stable while navigating through a folder.
    bool identical = files.size() == mFiles.size();
    if (identical) {
        for (int i = 0; i < files.size(); i++) {
            if (files.at(i).absoluteFilePath != mFiles.at(i).absoluteFilePath) {
                identical = false;
                break;
            }
        }
    }

    if (identical) {
        if (currentIndex == mCurrentRow)
            return false;
        const int oldRow = mCurrentRow;
        mCurrentRow = currentIndex;
        notifyRowChanged(oldRow);
        notifyRowChanged(mCurrentRow);
        return false;
    }

    beginResetModel();
    mFiles = files;
    mCurrentRow = currentIndex;
    mRowForPath.clear();
    mRowForPath.reserve(files.size());
    for (int i = 0; i < files.size(); i++)
        mRowForPath.insert(files.at(i).absoluteFilePath, i);
    endResetModel();
    return true;
}

QString QVThumbnailModel::filePath(int row) const
{
    return mFiles.value(row).absoluteFilePath;
}

int QVThumbnailModel::rowForPath(const QString &filePath) const
{
    return mRowForPath.value(filePath, -1);
}

void QVThumbnailModel::notifyRowChanged(int row)
{
    if (row < 0 || row >= mFiles.size())
        return;
    emit dataChanged(index(row), index(row));
}

// QVThumbnailDelegate

QVThumbnailDelegate::QVThumbnailDelegate(QVThumbnailBar *bar, QObject *parent)
    : QStyledItemDelegate(parent), mBar(bar)
{
}

void QVThumbnailDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                const QModelIndex &index) const
{
    const QString filePath = index.data(QVThumbnailModel::FilePathRole).toString();
    const bool isCurrent = index.data(QVThumbnailModel::CurrentRole).toBool();

    const QRect cellRect = option.rect;
    const QRect contentRect = cellRect.adjusted(QVThumbnailBar::CELL_PADDING,
                                                QVThumbnailBar::CELL_PADDING,
                                                -QVThumbnailBar::CELL_PADDING,
                                                -QVThumbnailBar::CELL_PADDING);

    const QColor backgroundColor = mBar->backgroundColor();
    const bool lightBackground = QVApplication::getPerceivedBrightness(backgroundColor) > 0.5;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    if (isCurrent) {
        QColor highlight = option.palette.highlight().color();
        highlight.setAlpha(70);
        painter->fillRect(cellRect, highlight);
    }

    const int pixelSize = mBar->currentPixelSize();
    const QImage image = mBar->getLoader()->cachedThumbnail(filePath, pixelSize);

    if (!image.isNull()) {
        QSize scaledSize = image.size();
        scaledSize.scale(contentRect.size(), Qt::KeepAspectRatio);
        const QRect drawRect(QPoint(contentRect.center().x() - scaledSize.width() / 2,
                                    contentRect.center().y() - scaledSize.height() / 2),
                             scaledSize);
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter->drawImage(drawRect, image);
    } else {
        // Placeholder while the thumbnail is decoding (or broken)
        const QColor borderColor =
                lightBackground ? QColor(0, 0, 0, 60) : QColor(255, 255, 255, 70);
        const QColor fillColor =
                lightBackground ? QColor(0, 0, 0, 20) : QColor(255, 255, 255, 25);

        painter->setPen(QPen(borderColor, 1));
        painter->setBrush(fillColor);
        painter->drawRoundedRect(contentRect, 4, 4);

        if (mBar->getLoader()->isCached(filePath, pixelSize)) {
            // Decoding failed for this file: draw a subtle X mark
            painter->setPen(QPen(borderColor, 2));
            const int inset = contentRect.width() / 4;
            const QRect markRect = contentRect.adjusted(inset, inset, -inset, -inset);
            painter->drawLine(markRect.topLeft(), markRect.bottomRight());
            painter->drawLine(markRect.topRight(), markRect.bottomLeft());
        }
    }

    if (isCurrent) {
        painter->setPen(QPen(option.palette.highlight(), 2));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(cellRect.adjusted(1, 1, -2, -2));
    }

    painter->restore();
}

QSize QVThumbnailDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(option)
    Q_UNUSED(index)
    return mBar->cellSize();
}

// QVThumbnailBar

QVThumbnailBar::QVThumbnailBar(QWidget *parent) : QWidget(parent)
{
    loader = new QVThumbnailLoader(this);
    model = new QVThumbnailModel(this);
    delegate = new QVThumbnailDelegate(this, this);

    listView = new QListView(this);
    listView->setModel(model);
    listView->setItemDelegate(delegate);
    listView->setViewMode(QListView::ListMode);
    listView->setFlow(QListView::LeftToRight);
    listView->setWrapping(false);
    listView->setUniformItemSizes(true);
    listView->setResizeMode(QListView::Adjust);
    listView->setSelectionMode(QListView::NoSelection);
    listView->setFocusPolicy(Qt::NoFocus);
    listView->setFrameShape(QFrame::NoFrame);
    // Cell padding is the gap between thumbnails. Extra spacing would make each
    // row taller than the fixed bar and clip the images.
    listView->setSpacing(0);
    listView->setContextMenuPolicy(Qt::NoContextMenu);
    listView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    listView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    listView->setAutoFillBackground(false);
    listView->viewport()->setAutoFillBackground(false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(BAR_MARGIN, BAR_MARGIN, BAR_MARGIN, BAR_MARGIN);
    layout->setSpacing(0);
    layout->addWidget(listView);

    // Clicking a thumbnail loads that image
    connect(listView, &QListView::clicked, this, [this](const QModelIndex &index) {
        if (!index.isValid() || index.row() == model->currentRow())
            return;
        const QString path = model->filePath(index.row());
        if (!path.isEmpty())
            emit fileRequested(path);
    });

    // Repaint items whose thumbnails finished loading
    connect(loader, &QVThumbnailLoader::thumbnailLoaded, this,
            [this](const QString &filePath, int pixelSize) {
                if (pixelSize != currentPixelSize())
                    return;
                const int row = model->rowForPath(filePath);
                if (row >= 0)
                    model->notifyRowChanged(row);
            });

    // Load thumbnails for newly revealed items while scrolling
    connect(listView->horizontalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int) { requestVisibleThumbnails(); });
    connect(listView->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int) { requestVisibleThumbnails(); });

    listView->viewport()->installEventFilter(this);

    settingsUpdated();
}

void QVThumbnailBar::syncFromCore(const QVImageCore::FileDetails &details)
{
    const QString dir = details.fileInfo.path();
    if (!dir.isEmpty() && dir != lastDir) {
        lastDir = dir;
        loader->cancelPending();
    }

    model->setFileList(details.folderFileInfoList, details.loadedIndexInFolder);

    if (model->currentRow() >= 0)
        listView->scrollTo(model->index(model->currentRow()), QAbstractItemView::EnsureVisible);

    requestVisibleThumbnails();
}

void QVThumbnailBar::refreshThumbnails()
{
    requestVisibleThumbnails();
    listView->viewport()->update();
}

void QVThumbnailBar::settingsUpdated()
{
    auto &settingsManager = qvApp->getSettingsManager();

    mCustomBackgroundColor = settingsManager.getBool(SettingsManager::Setting::BgColorEnabled)
            ? QColor(settingsManager.getString(SettingsManager::Setting::BgColor))
            : QColor();

    const int sizeSetting = settingsManager.getInt(SettingsManager::Setting::ThumbnailBarSize);
    if (sizeSetting != mSizeSetting) {
        // mSizeSetting starts at -1 so the first read always builds the grid,
        // including the default "medium" size.
        const bool sizeChanged = mSizeSetting >= 0;
        mSizeSetting = sizeSetting;
        if (sizeChanged)
            loader->clearCache();
        rebuildGeometries();
        requestVisibleThumbnails();
    }

    update();
}

void QVThumbnailBar::setPosition(int position)
{
    const bool horizontal = position == static_cast<int>(Position::Bottom);
    if (horizontal == mHorizontal)
        return;

    mHorizontal = horizontal;
    listView->setFlow(horizontal ? QListView::LeftToRight : QListView::TopToBottom);
    listView->setHorizontalScrollBarPolicy(horizontal ? Qt::ScrollBarAsNeeded
                                                      : Qt::ScrollBarAlwaysOff);
    listView->setVerticalScrollBarPolicy(horizontal ? Qt::ScrollBarAlwaysOff
                                                    : Qt::ScrollBarAsNeeded);
    rebuildGeometries();
    requestVisibleThumbnails();
}

void QVThumbnailBar::setTitlebarInset(int inset)
{
    inset = qMax(0, inset);
    if (inset == mTitlebarInset)
        return;

    mTitlebarInset = inset;
    if (!mHorizontal)
        rebuildGeometries();
}

int QVThumbnailBar::thumbnailLogicalSize() const
{
    switch (mSizeSetting) {
    case 0:
        return 48;
    case 2:
        return 112;
    default:
        return 80;
    }
}

int QVThumbnailBar::currentPixelSize() const
{
    return qRound(thumbnailLogicalSize() * devicePixelRatioF());
}

QColor QVThumbnailBar::backgroundColor() const
{
    return mCustomBackgroundColor.isValid() ? mCustomBackgroundColor
                                            : palette().color(QPalette::Window);
}

void QVThumbnailBar::rebuildGeometries()
{
    const int logicalSize = thumbnailLogicalSize();
    mCellSize = QSize(logicalSize + CELL_PADDING * 2, logicalSize + CELL_PADDING * 2);
    listView->setGridSize(mCellSize);

    // A vertical bar starts under the transparent titlebar; keep thumbnails below it.
    const int topMargin = BAR_MARGIN + (mHorizontal ? 0 : mTitlebarInset);
    layout()->setContentsMargins(BAR_MARGIN, topMargin, BAR_MARGIN, BAR_MARGIN);

    // Non-overlay scrollbars take space inside the fixed extent and would clip cells.
    int scrollExtent = 0;
    if (!style()->styleHint(QStyle::SH_ScrollBar_Transient, nullptr, this))
        scrollExtent = style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this);

    setMinimumSize(0, 0);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    if (mHorizontal)
        setFixedHeight(mCellSize.height() + BAR_MARGIN * 2 + scrollExtent);
    else
        setFixedWidth(mCellSize.width() + BAR_MARGIN * 2 + scrollExtent);
}

void QVThumbnailBar::requestVisibleThumbnails()
{
    if (model->rowCount() == 0)
        return;

    const QSize grid = listView->gridSize();
    const int step = (mHorizontal ? grid.width() : grid.height()) + listView->spacing();
    if (step <= 0)
        return;

    const QWidget *viewport = listView->viewport();
    int first = -1;
    int last = -1;

    const auto recordIndex = [&](const QModelIndex &index) {
        if (index.isValid()) {
            if (first == -1)
                first = index.row();
            last = index.row();
        }
    };

    if (mHorizontal) {
        const int y = qMin(viewport->height() - 1, grid.height() / 2);
        for (int x = 0; x < viewport->width(); x += step)
            recordIndex(listView->indexAt(QPoint(x, y)));
        recordIndex(listView->indexAt(QPoint(viewport->width() - 1, y)));
    } else {
        const int x = qMin(viewport->width() - 1, grid.width() / 2);
        for (int y = 0; y < viewport->height(); y += step)
            recordIndex(listView->indexAt(QPoint(x, y)));
        recordIndex(listView->indexAt(QPoint(x, viewport->height() - 1)));
    }

    if (first == -1)
        return;

    first = qMax(0, first - VISIBLE_BUFFER);
    last = qMin(model->rowCount() - 1, last + VISIBLE_BUFFER);

    const int pixelSize = currentPixelSize();
    for (int row = first; row <= last; row++) {
        const QString path = model->filePath(row);
        if (!loader->isCached(path, pixelSize))
            loader->requestThumbnail(path, pixelSize);
    }
}

bool QVThumbnailBar::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == listView->viewport()) {
        if (event->type() == QEvent::Resize) {
            requestVisibleThumbnails();
        } else if (event->type() == QEvent::Wheel && mHorizontal) {
            // Route wheel/trackpad scrolling to the horizontal scrollbar
            auto *wheelEvent = static_cast<QWheelEvent *>(event);
            QScrollBar *scrollBar = listView->horizontalScrollBar();
            if (scrollBar->maximum() > scrollBar->minimum()) {
                // Trackpads report pixelDelta. A mouse wheel often reports only angleDelta.
                const QPoint deltaPoint = wheelEvent->pixelDelta().isNull()
                        ? wheelEvent->angleDelta()
                        : wheelEvent->pixelDelta();
                const int delta = qAbs(deltaPoint.y()) >= qAbs(deltaPoint.x()) ? deltaPoint.y()
                                                                               : deltaPoint.x();
                if (delta != 0) {
                    scrollBar->setValue(scrollBar->value() - delta);
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void QVThumbnailBar::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.fillRect(rect(), backgroundColor());

    // Separator line towards the image view
    const QColor backgroundColor = this->backgroundColor();
    const QColor separatorColor = QVApplication::getPerceivedBrightness(backgroundColor) > 0.5
            ? QColor(0, 0, 0, 60)
            : QColor(255, 255, 255, 60);
    painter.setPen(QPen(separatorColor, 1));
    if (mHorizontal)
        painter.drawLine(0, 0, width() - 1, 0);
    else
        painter.drawLine(width() - 1, 0, width() - 1, height() - 1);
}
