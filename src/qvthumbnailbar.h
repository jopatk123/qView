#ifndef QVTHUMBNAILBAR_H
#define QVTHUMBNAILBAR_H

#include "qvimagecore.h"
#include "qvthumbnailloader.h"

#include <QAbstractListModel>
#include <QStyledItemDelegate>
#include <QWidget>

class QListView;

class QVThumbnailModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles
    {
        FilePathRole = Qt::UserRole + 1,
        CurrentRole
    };

    explicit QVThumbnailModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    // Returns true when the list itself changed (model was reset)
    bool setFileList(const QList<QVImageCore::CompatibleFile> &files, int currentIndex);
    int currentRow() const { return mCurrentRow; }
    QString filePath(int row) const;
    int rowForPath(const QString &filePath) const;
    void notifyRowChanged(int row);

private:
    QList<QVImageCore::CompatibleFile> mFiles;
    QHash<QString, int> mRowForPath;
    int mCurrentRow = -1;
};

class QVThumbnailBar;

class QVThumbnailDelegate : public QStyledItemDelegate
{
public:
    explicit QVThumbnailDelegate(QVThumbnailBar *bar, QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    QVThumbnailBar *mBar;
};

class QVThumbnailBar : public QWidget
{
    Q_OBJECT
public:
    // Positions must match the ThumbnailBarPosition setting
    enum class Position
    {
        Hidden = 0,
        Bottom = 1,
        Left = 2
    };

    explicit QVThumbnailBar(QWidget *parent = nullptr);

    // Re-sync the thumbnail list with the image core's current file details
    void syncFromCore(const QVImageCore::FileDetails &details);

    // Re-read size and color settings
    void settingsUpdated();

    // 1 = bottom (horizontal), 2 = left (vertical)
    void setPosition(int position);

    // Pixels of the macOS titlebar that cover the top of a vertical bar
    void setTitlebarInset(int inset);

    int currentPixelSize() const;
    QColor backgroundColor() const;
    QVThumbnailLoader *getLoader() const { return loader; }
    const QSize &cellSize() const { return mCellSize; }

    static const int CELL_PADDING = 4;
    static const int BAR_MARGIN = 6;

signals:
    void fileRequested(const QString &filePath);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    int thumbnailLogicalSize() const;
    void rebuildGeometries();
    void requestVisibleThumbnails();

    QVThumbnailLoader *loader;
    QVThumbnailModel *model;
    QVThumbnailDelegate *delegate;
    QListView *listView;

    QString lastDir;
    QSize mCellSize;
    QColor mCustomBackgroundColor;
    int mSizeSetting = -1;
    int mTitlebarInset = 0;
    bool mHorizontal = true;

    static const int VISIBLE_BUFFER = 12;
};

#endif // QVTHUMBNAILBAR_H
