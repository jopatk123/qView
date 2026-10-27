#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "qvinfodialog.h"
#include "qvimagecore.h"
#include "qvgraphicsview.h"
#include "openwith.h"

#include <QMainWindow>
#include <QShortcut>
#include <QNetworkAccessManager>
#include <QStack>

namespace Ui {
class MainWindow;
}

class QAction;
class QHBoxLayout;
class QLabel;
class QToolButton;
class QVBoxLayout;
class QVFileInfoPanel;
class QVThumbnailBar;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    struct DeletedPaths
    {
        QString pathInTrash;
        QString previousPath;
    };

    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void requestPopulateOpenWithMenu();

    void populateOpenWithMenu(const QList<OpenWith::OpenWithItem> openWithItems);

    void refreshProperties();

    void updateWindowTitle();

    void updateWindowFilePath();

    void setWindowSize();

    bool getIsPixmapLoaded() const;

    void setJustLaunchedWithImage(bool value);

    QScreen *screenContaining(const QRect &rect);

    void openRecent(int i);

    void openUrl(const QUrl &url);

    void pickUrl();

    void reloadFile();

    void openContainingFolder();

    void openWith(const OpenWith::OpenWithItem &exec);

    void showFileInfo();

    // showNext forces the following image after a successful delete. The menu
    // actions leave it false and follow the After Deletion setting.
    void askDeleteFile(bool permanent, bool showNext = false);

    void deleteFile(bool permanent, bool showNext = false);

    QString deleteFileLinuxFallback(const QString &path, bool putBack);

    void undoDelete();

    void copy();

    void paste();

    void rename();

    // Renames the current file to a new name in the same folder, used by the
    // file info panel when its name row is edited in place
    void renameCurrentFileTo(const QString &newFileName);

    void zoomIn();

    void zoomOut();

    void resetZoom();

    void originalSize();

    void rotateRight();

    void rotateLeft();

    void mirror();

    void flip();

    void saveImage();

    void printImage();

    void exportPdf();

    void revertImage();

    void cropImage();

    void compressImage();

    void batchCompress();

    void firstFile();

    void previousFile();

    void nextFile();

    void lastFile();

    void saveFrameAs();

    void pause();

    void nextFrame();

    void decreaseSpeed();

    void resetSpeed();

    void increaseSpeed();

    void toggleFullScreen();

    void toggleThumbnailBar();

    void toggleFileInfoPanel();

    void toggleThumbnailBarVisible();

    void toggleFileInfoPanelVisible();

    int getTitlebarOverlap() const;

    const QVImageCore::FileDetails &getCurrentFileDetails() const
    {
        return graphicsView->getCurrentFileDetails();
    }

public slots:
    void openFile(const QString &fileName);

    void toggleSlideshow();

    void slideshowAction();

    void cancelSlideshow();

    void fileChanged();

    void disableActions();

protected:
    bool event(QEvent *event) override;

    void contextMenuEvent(QContextMenuEvent *event) override;

    void showEvent(QShowEvent *event) override;

    void closeEvent(QCloseEvent *event) override;

    void changeEvent(QEvent *event) override;

    void mousePressEvent(QMouseEvent *event) override;

    void mouseDoubleClickEvent(QMouseEvent *event) override;

    void paintEvent(QPaintEvent *event) override;

    void fullscreenChanged();

protected slots:
    void settingsUpdated();
    void shortcutsUpdated();

private:
    Ui::MainWindow *ui;
    QVGraphicsView *graphicsView;

    QVThumbnailBar *thumbnailBar;
    QVFileInfoPanel *fileInfoPanel;
    QWidget *imageColumn = nullptr;
    QVBoxLayout *imageColumnLayout = nullptr;
    QWidget *imageToolbar = nullptr;
    QToolButton *openButton = nullptr;
    QToolButton *restoreButton = nullptr;
    QToolButton *saveButton = nullptr;
    QLabel *compressResultLabel = nullptr;
    QToolButton *cropButton = nullptr;
    QToolButton *compressButton = nullptr;
    QAction *compressCurrentAction = nullptr;
    QToolButton *rotateRightButton = nullptr;
    QToolButton *thumbnailBarButton = nullptr;
    QToolButton *fileInfoButton = nullptr;
    QToolButton *trashButton = nullptr;
    QToolButton *printButton = nullptr;
    QToolButton *pdfButton = nullptr;
    QHBoxLayout *imageRowLayout = nullptr;
    int appliedThumbnailPosition = 0;
    int appliedInfoPosition = 0;

    QMenu *contextMenu;
    QMenu *virtualMenu;

    QTimer *slideshowTimer;

    QShortcut *escShortcut;

    QVInfoDialog *info;

    QColor customBackgroundColor;

    bool justLaunchedWithImage;

    Qt::WindowStates storedWindowState;

    QNetworkAccessManager networkAccessManager;

    QStack<DeletedPaths> lastDeletedFiles;

    QTimer *populateOpenWithTimer;
    QFutureWatcher<QList<OpenWith::OpenWithItem>> openWithFutureWatcher;

    void updateFileInfo(bool updateDialog, bool updatePanel);

    void updateThumbnailBarPosition();

    void setupImageToolbar();

    void updateImageToolbar();

    void rememberVisiblePanels();

    void toggleRememberedPosition(const QString &positionKey, const QString &lastVisibleKey,
                                  int current, int fallbackVisible);

    static QString actionToolTip(const QString &key);
};

#endif // MAINWINDOW_H
