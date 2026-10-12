#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "qvapplication.h"
#include "qvcocoafunctions.h"
#include "qvrenamedialog.h"
#include "qvthumbnailbar.h"
#include "qvfileinfopanel.h"
#include "qvbatchcompressdialog.h"

#include <QHBoxLayout>
#include <QVBoxLayout>

#include <QFileDialog>
#include <QMessageBox>
#include <QString>
#include <QGraphicsPixmapItem>
#include <QPixmap>
#include <QClipboard>
#include <QCoreApplication>
#include <QFileSystemWatcher>
#include <QProcess>
#include <QDesktopServices>
#include <QContextMenuEvent>
#include <QMovie>
#include <QImageWriter>
#include <QSettings>
#include <QStyle>
#include <QIcon>
#include <QMimeDatabase>
#include <QScreen>
#include <QCursor>
#include <QInputDialog>
#include <QProgressDialog>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QMenu>
#include <QToolButton>
#include <QWindow>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSlider>
#include <QSpinBox>
#include <QLabel>
#include <QComboBox>
#include <QTimer>
#include <QLocale>
#include <memory>
#include <QPushButton>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QTemporaryFile>

namespace {

const int kImageToolbarHeight = 34;
const int kImageToolbarIconSize = 18;

QString formatByteCount(qint64 bytes)
{
    return QLocale().formattedDataSize(qMax<qint64>(0, bytes));
}

QString compressionSummary(qint64 before, qint64 after)
{
    const QString afterText = formatByteCount(after);
    if (before <= 0)
        return MainWindow::tr("Compressed size: %1").arg(afterText);

    const qint64 delta = before - after;
    if (delta == 0)
        return MainWindow::tr("%1 → %2, same size").arg(formatByteCount(before), afterText);

    const int percent = int((qAbs(delta) * 100 + before / 2) / before);
    const QString deltaText = formatByteCount(qAbs(delta));
    const QString beforeText = formatByteCount(before);
    if (delta > 0) {
        return MainWindow::tr("%1 → %2, reduced %3 (%4%%)")
                .arg(beforeText, afterText, deltaText, QString::number(percent));
    }
    return MainWindow::tr("%1 → %2, increased %3 (%4%%)")
            .arg(beforeText, afterText, deltaText, QString::number(percent));
}

// Icons sit in the strip under the macOS titlebar. Empty space there still drags the window.
class ImageToolbar : public QWidget
{
public:
    explicit ImageToolbar(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(kImageToolbarHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            const auto state = window()->windowState();
            if (!state.testFlag(Qt::WindowFullScreen) && !state.testFlag(Qt::WindowMaximized)) {
#ifdef COCOA_LOADED
                if (QVCocoaFunctions::startSystemMove(window()))
                    return;
#elif QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
                if (window()->windowHandle() && window()->windowHandle()->startSystemMove())
                    return;
#else
                Q_UNUSED(event)
#endif
            }
        }
        QWidget::mousePressEvent(event);
    }
};

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_OpaquePaintEvent);

#if defined COCOA_LOADED && QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // Allow the titlebar to overlap widgets with full size content view
    setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    centralWidget()->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
#endif

    // Initialize variables
    justLaunchedWithImage = false;
    storedWindowState = Qt::WindowNoState;

    // Initialize graphicsviewkDefaultBufferAlignment
    graphicsView = new QVGraphicsView(this);
    setupImageToolbar();

    // Initialize thumbnail bar
    thumbnailBar = new QVThumbnailBar(this);
    thumbnailBar->hide();
    connect(thumbnailBar, &QVThumbnailBar::fileRequested, this, &MainWindow::openFile);

    // Persistent file info, shown when the matching setting is not hidden
    fileInfoPanel = new QVFileInfoPanel(this);
    fileInfoPanel->hide();

    // Hide fullscreen label by default
    ui->fullscreenLabel->hide();

    // Connect graphicsview signals
    connect(graphicsView, &QVGraphicsView::fileChanged, this, &MainWindow::fileChanged);
    connect(graphicsView, &QVGraphicsView::imageStateChanged, this, [this]() {
        updateImageToolbar();
        updateWindowTitle();
        refreshProperties();
    });
    connect(graphicsView, &QVGraphicsView::cropModeChanged, this, [this](bool) {
        updateImageToolbar();
    });
    connect(graphicsView, &QVGraphicsView::updatedLoadedPixmapItem, this,
            &MainWindow::setWindowSize);
    connect(graphicsView, &QVGraphicsView::cancelSlideshow, this, &MainWindow::cancelSlideshow);

    // Initialize escape shortcut
    escShortcut = new QShortcut(Qt::Key_Escape, this);
    connect(escShortcut, &QShortcut::activated, this, [this]() {
        if (graphicsView->isCropping()) {
            graphicsView->cancelCrop();
            return;
        }
        if (windowState().testFlag(Qt::WindowFullScreen))
            toggleFullScreen();
    });

    // Enable drag&dropping
    setAcceptDrops(true);

    // Make info dialog object
    info = new QVInfoDialog(this);

    // Timer for slideshow
    slideshowTimer = new QTimer(this);
    connect(slideshowTimer, &QTimer::timeout, this, &MainWindow::slideshowAction);

    // Context menu
    auto &actionManager = qvApp->getActionManager();

    contextMenu = new QMenu(this);

    actionManager.addCloneOfAction(contextMenu, "open");
    actionManager.addCloneOfAction(contextMenu, "openurl");
    contextMenu->addMenu(actionManager.buildRecentsMenu(true, contextMenu));
    contextMenu->addMenu(actionManager.buildOpenWithMenu(contextMenu));
    actionManager.addCloneOfAction(contextMenu, "opencontainingfolder");
    actionManager.addCloneOfAction(contextMenu, "showfileinfo");
    contextMenu->addSeparator();
    actionManager.addCloneOfAction(contextMenu, "rename");
    actionManager.addCloneOfAction(contextMenu, "delete");
    contextMenu->addSeparator();
    actionManager.addCloneOfAction(contextMenu, "nextfile");
    actionManager.addCloneOfAction(contextMenu, "previousfile");
    contextMenu->addSeparator();
    contextMenu->addMenu(actionManager.buildViewMenu(true, contextMenu));
    contextMenu->addMenu(actionManager.buildToolsMenu(true, contextMenu, false));
    contextMenu->addSeparator();
    actionManager.addCloneOfAction(contextMenu, "options");
    contextMenu->addMenu(actionManager.buildHelpMenu(true, contextMenu));

    connect(contextMenu, &QMenu::triggered, this, [this](QAction *triggeredAction) {
        ActionManager::actionTriggered(triggeredAction, this);
    });

    // Initialize menubar
    setMenuBar(actionManager.buildMenuBar(this));
    // Stop actions conflicting with the window's actions
    const auto menubarActions = ActionManager::getAllNestedActions(menuBar()->actions());
    for (auto action : menubarActions) {
        action->setShortcutContext(Qt::WidgetShortcut);
    }
    connect(menuBar(), &QMenuBar::triggered, this, [this](QAction *triggeredAction) {
        ActionManager::actionTriggered(triggeredAction, this);
    });

    // Add all actions to this window so keyboard shortcuts are always triggered
    // using virtual menu to hold them so i can connect to the triggered signal
    virtualMenu = new QMenu(this);
    const auto &actionKeys = actionManager.getActionLibrary().keys();
    for (const QString &key : actionKeys) {
        actionManager.addCloneOfAction(virtualMenu, key);
    }
    addActions(virtualMenu->actions());
    connect(virtualMenu, &QMenu::triggered, this, [this](QAction *triggeredAction) {
        ActionManager::actionTriggered(triggeredAction, this);
    });

    // Enable actions related to having a window
    disableActions();

    // Connect functions to application components
    connect(&qvApp->getShortcutManager(), &ShortcutManager::shortcutsUpdated, this,
            &MainWindow::shortcutsUpdated);
    connect(&qvApp->getSettingsManager(), &SettingsManager::settingsUpdated, this,
            &MainWindow::settingsUpdated);
    settingsUpdated();
    shortcutsUpdated();

    // Timer for delayed-load Open With menu
    populateOpenWithTimer = new QTimer(this);
    populateOpenWithTimer->setSingleShot(true);
    populateOpenWithTimer->setInterval(250);
    connect(populateOpenWithTimer, &QTimer::timeout, this,
            &MainWindow::requestPopulateOpenWithMenu);

    // Connection for open with menu population futurewatcher
    connect(&openWithFutureWatcher, &QFutureWatcher<QList<OpenWith::OpenWithItem>>::finished, this,
            [this]() { populateOpenWithMenu(openWithFutureWatcher.result()); });

    // Load window geometry
    QSettings settings;
    restoreGeometry(settings.value("geometry").toByteArray());

    // Show welcome dialog on first launch
    if (!settings.value("firstlaunch", false).toBool()) {
        settings.setValue("firstlaunch", true);
        settings.setValue("configversion", VERSION);
        qvApp->openWelcomeDialog(this);
    }
}

MainWindow::~MainWindow()
{
    delete ui;
}

bool MainWindow::event(QEvent *event)
{
    if (event->type() == QEvent::WindowActivate) {
        qvApp->addToLastActiveWindows(this);
    }
    return QMainWindow::event(event);
}

void MainWindow::contextMenuEvent(QContextMenuEvent *event)
{
    QMainWindow::contextMenuEvent(event);

    // Show native menu on macOS with cocoa framework loaded
#ifdef COCOA_LOADED
    // On regular context menu, recents submenu updates right before it is shown.
    // The native cocoa menu does not update elements until the entire menu is reopened, so we
    // update first
    qvApp->getActionManager().loadRecentsList();
    QVCocoaFunctions::showMenu(contextMenu, event->pos(), windowHandle());
#else
    contextMenu->popup(event->globalPos());
#endif
}

void MainWindow::showEvent(QShowEvent *event)
{
#ifdef COCOA_LOADED
    // Enable full size content view. With some Qt versions, this can break its restoreGeometry
    // functionality even if we enable this after. Presumably restoreGeometry saves data for
    // further processing after the window is shown, hence the timer here to queue this on the
    // event loop and run after that processing happens.
    QTimer::singleShot(0, this, [this]() {
        QVCocoaFunctions::setFullSizeContentView(windowHandle(), true);
        // The titlebar overlap is only known after the full-size content view is on.
        updateThumbnailBarPosition();
    });
#endif

    if (!menuBar()->sizeHint().isEmpty()) {
        ui->fullscreenLabel->setMargin(0);
        ui->fullscreenLabel->setMinimumHeight(menuBar()->sizeHint().height());
    }

    QMainWindow::showEvent(event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
#ifdef COCOA_LOADED
    // Full size content view can confuse saveGeometry, making it think the window is taller than
    // it really is, so the restored window ends up taller. Turn it off before saving geometry.
    QVCocoaFunctions::setFullSizeContentView(windowHandle(), false);
#endif

    QSettings settings;
    settings.setValue("geometry", saveGeometry());

    qvApp->deleteFromLastActiveWindows(this);
    qvApp->getActionManager().untrackClonedActions(contextMenu);
    qvApp->getActionManager().untrackClonedActions(menuBar());
    qvApp->getActionManager().untrackClonedActions(virtualMenu);

    QMainWindow::closeEvent(event);
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::WindowStateChange) {
        const auto *changeEvent = static_cast<QWindowStateChangeEvent *>(event);
        if (windowState().testFlag(Qt::WindowFullScreen)
            != changeEvent->oldState().testFlag(Qt::WindowFullScreen))
            fullscreenChanged();
    }

    QMainWindow::changeEvent(event);
}

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MouseButton::BackButton)
        previousFile();
    else if (event->button() == Qt::MouseButton::ForwardButton)
        nextFile();
    else if (event->button() == Qt::MouseButton::MiddleButton)
        resetZoom();

    QMainWindow::mousePressEvent(event);
}

void MainWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MouseButton::LeftButton)
        toggleFullScreen();
    QMainWindow::mouseDoubleClickEvent(event);
}

void MainWindow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);

    const QColor &backgroundColor =
            customBackgroundColor.isValid() ? customBackgroundColor : painter.background().color();

    // Find the top of the viewport to account for the menu bar if it's inside the window
    // and/or the label that displays titlebar text in full screen mode.
    const int viewportY = graphicsView->mapTo(this, QPoint()).y();
    // On macOS, part of the viewport may be additionally covered with the window's translucent
    // titlebar due to full size content view.
    const int unobscuredViewportY = qMax(getTitlebarOverlap(), viewportY);

    // Erase the area above the viewport, i.e. fill it with the painter's default color.
    const QRect headerRect = QRect(0, 0, width(), viewportY);
    if (headerRect.isValid()) {
        painter.eraseRect(headerRect);
    }

    // Fill the viewport with the background color.
    const QRect viewportRect = rect().adjusted(0, viewportY, 0, 0);
    if (viewportRect.isValid()) {
        painter.fillRect(viewportRect, backgroundColor);
    }

    // If there's an error message, draw it centered inside the unobscured area of the viewport.
    const QRect unobscuredViewportRect = rect().adjusted(0, unobscuredViewportY, 0, 0);
    if (getCurrentFileDetails().errorData.hasError && unobscuredViewportRect.isValid()) {
        const QVImageCore::ErrorData &errorData = getCurrentFileDetails().errorData;
        const QString errorMessage =
                tr("Error occurred opening\n%3\n%2 (Error %1)")
                        .arg(QString::number(errorData.errorNum), errorData.errorString,
                             getCurrentFileDetails().fileInfo.fileName());
        painter.setFont(font());
        painter.setPen(QVApplication::getPerceivedBrightness(backgroundColor) > 0.5 ? Qt::black
                                                                                    : Qt::white);
        painter.drawText(unobscuredViewportRect, errorMessage, QTextOption(Qt::AlignCenter));
    }
    QMainWindow::paintEvent(event);
}

void MainWindow::fullscreenChanged()
{
    const bool isFullscreen = windowState().testFlag(Qt::WindowFullScreen);
    const auto fullscreenActions =
            qvApp->getActionManager().getAllClonesOfAction("fullscreen", this);
    for (const auto &fullscreenAction : fullscreenActions) {
        fullscreenAction->setText(isFullscreen ? tr("Exit F&ull Screen")
                                               : tr("Enter F&ull Screen"));
        fullscreenAction->setIcon(isFullscreen ? QIcon::fromTheme("view-restore")
                                               : QIcon::fromTheme("view-fullscreen"));
    }
    ui->fullscreenLabel->setVisible(
            isFullscreen
            && qvApp->getSettingsManager().getBool(SettingsManager::Setting::FullScreenDetails));

    updateThumbnailBarPosition();
}

void MainWindow::openFile(const QString &fileName)
{
    graphicsView->loadFile(fileName);
    cancelSlideshow();
}

void MainWindow::settingsUpdated()
{
    auto &settingsManager = qvApp->getSettingsManager();

    updateWindowTitle();

    // Cache custom background color since we read it in paintEvent
    customBackgroundColor = settingsManager.getBool(SettingsManager::Setting::BgColorEnabled)
            ? QColor(settingsManager.getString(SettingsManager::Setting::BgColor))
            : QColor();

    // menubarenabled
    bool menuBarEnabled = settingsManager.getBool(SettingsManager::Setting::MenuBarEnabled);
#ifdef Q_OS_MACOS
    // Menu bar is effectively always enabled on macOS
    menuBarEnabled = true;
#endif
    menuBar()->setVisible(menuBarEnabled);

#ifdef COCOA_LOADED
    // titlebaralwaysdark/hidetitlebar
    QVCocoaFunctions::setVibrancy(
            settingsManager.getBool(SettingsManager::Setting::HideTitlebar),
            settingsManager.getBool(SettingsManager::Setting::ForceDarkMode), windowHandle());
    // quitonlastwindow
    qvApp->setQuitOnLastWindowClosed(
            settingsManager.getBool(SettingsManager::Setting::QuitOnLastWindow));
#endif

    // slideshow timer
    slideshowTimer->setInterval(static_cast<int>(
            settingsManager.getDouble(SettingsManager::Setting::SlideshowTimer) * 1000));

    ui->fullscreenLabel->setVisible(
            settingsManager.getBool(SettingsManager::Setting::FullScreenDetails)
            && windowState().testFlag(Qt::WindowFullScreen));

    rememberVisiblePanels();
    thumbnailBar->settingsUpdated();
    fileInfoPanel->settingsUpdated();
    if (qvGetSettingInt(FileInfoPanelPosition) != 0)
        refreshProperties();
    updateThumbnailBarPosition();

    setWindowSize();

    // repaint in case background color changed
    update();
}

void MainWindow::shortcutsUpdated()
{
    // If esc is not used in a shortcut, let it exit fullscreen
    escShortcut->setKey(Qt::Key_Escape);

    const auto &actionLibrary = qvApp->getActionManager().getActionLibrary();
    for (const auto &action : actionLibrary) {
        if (action->shortcuts().contains(QKeySequence(Qt::Key_Escape))) {
            escShortcut->setKey({});
            break;
        }
    }
}

void MainWindow::openRecent(int i)
{
    auto recentsList = qvApp->getActionManager().getRecentsList();
    graphicsView->loadFile(recentsList.value(i).filePath);
    cancelSlideshow();
}

void MainWindow::fileChanged()
{
    populateOpenWithTimer->start();
    disableActions();

    // Sync the thumbnail bar with the current file list
    thumbnailBar->syncFromCore(getCurrentFileDetails());
    refreshProperties();
    updateThumbnailBarPosition();
    updateWindowTitle();
    updateWindowFilePath();

    // repaint to handle error message
    update();
}

void MainWindow::disableActions()
{
    const auto &actionLibrary = qvApp->getActionManager().getActionLibrary();
    for (const auto &action : actionLibrary) {
        const auto &data = action->data().toStringList();
        const auto &clonesOfAction =
                qvApp->getActionManager().getAllClonesOfAction(data.first(), this);

        // Enable this window's actions when a file is loaded
        if (data.last().contains("disable")) {
            for (const auto &clone : clonesOfAction) {
                const auto &cloneData = clone->data().toStringList();
                if (cloneData.last() == "disable") {
                    clone->setEnabled(getCurrentFileDetails().isPixmapLoaded);
                } else if (cloneData.last() == "gifdisable") {
                    clone->setEnabled(getCurrentFileDetails().isMovieLoaded);
                } else if (cloneData.last() == "undodisable") {
                    clone->setEnabled(!lastDeletedFiles.isEmpty()
                                      && !lastDeletedFiles.top().pathInTrash.isEmpty());
                } else if (cloneData.last() == "folderdisable") {
                    clone->setEnabled(!getCurrentFileDetails().folderFileInfoList.isEmpty());
                } else if (cloneData.last() == "windowdisable") {
                    clone->setEnabled(true);
                }
            }
        }
    }

    const auto &openWithMenus = qvApp->getActionManager().getAllClonesOfMenu("openwith", this);
    for (const auto &menu : openWithMenus) {
        menu->setEnabled(getCurrentFileDetails().isPixmapLoaded);
    }
}

void MainWindow::requestPopulateOpenWithMenu()
{
    openWithFutureWatcher.setFuture(QtConcurrent::run([&] {
        const auto &curFilePath = getCurrentFileDetails().fileInfo.absoluteFilePath();
        return OpenWith::getOpenWithItems(curFilePath);
    }));
}

void MainWindow::populateOpenWithMenu(const QList<OpenWith::OpenWithItem> openWithItems)
{
    for (int i = 0; i < qvApp->getActionManager().getOpenWithMaxLength(); i++) {
        const auto clonedActions = qvApp->getActionManager().getAllClonesOfAction(
                "openwith" + QString::number(i), this);
        for (const auto &action : clonedActions) {
            // If we are within the bounds of the open with list
            if (i < openWithItems.length()) {
                auto openWithItem = openWithItems.value(i);

                action->setVisible(true);
                action->setIconVisibleInMenu(
                        false); // Hide icon temporarily to speed up updates in certain cases
                action->setText(openWithItem.name);
                if (!openWithItem.iconName.isEmpty())
                    action->setIcon(QIcon::fromTheme(openWithItem.iconName));
                else
                    action->setIcon(openWithItem.icon);
                auto data = action->data().toList();
                data.replace(1, QVariant::fromValue(openWithItem));
                action->setData(data);
                action->setIconVisibleInMenu(true);
            } else {
                action->setVisible(false);
            }
        }
    }
}

void MainWindow::updateFileInfo(bool updateDialog, bool updatePanel)
{
    if (!updateDialog && !updatePanel)
        return;

    int frameCount = 0;
    if (getCurrentFileDetails().isMovieLoaded)
        frameCount = graphicsView->getLoadedMovie().frameCount();

    const QFileInfo &fileInfo = getCurrentFileDetails().fileInfo;
    const int width = getCurrentFileDetails().baseImageSize.width();
    const int height = getCurrentFileDetails().baseImageSize.height();
    if (updateDialog)
        info->setInfo(fileInfo, width, height, frameCount);
    if (updatePanel)
        fileInfoPanel->setInfo(fileInfo, width, height, frameCount);
}

void MainWindow::refreshProperties()
{
    updateFileInfo(info->isVisible(), qvGetSettingInt(FileInfoPanelPosition) != 0);
}

void MainWindow::updateWindowTitle()
{
    QString newString = "qView";
    if (getCurrentFileDetails().fileInfo.isFile()) {
        switch (qvApp->getSettingsManager().getInt(SettingsManager::Setting::TitleBarMode)) {
        case 1: {
            newString = getCurrentFileDetails().fileInfo.fileName();
            break;
        }
        case 2: {
            newString = QString::number(getCurrentFileDetails().loadedIndexInFolder + 1);
            newString += "/" + QString::number(getCurrentFileDetails().folderFileInfoList.count());
            newString += " - " + getCurrentFileDetails().fileInfo.fileName();
            break;
        }
        case 3: {
            newString = QString::number(getCurrentFileDetails().loadedIndexInFolder + 1);
            newString += "/" + QString::number(getCurrentFileDetails().folderFileInfoList.count());
            newString += " - " + getCurrentFileDetails().fileInfo.fileName();
            if (!getCurrentFileDetails().errorData.hasError) {
                newString += " - " + QString::number(getCurrentFileDetails().baseImageSize.width());
                newString += "x" + QString::number(getCurrentFileDetails().baseImageSize.height());
                newString +=
                        " - " + QVInfoDialog::formatBytes(getCurrentFileDetails().fileInfo.size());
            }
            newString += " - qView";
            break;
        }
        }
    }

    setWindowTitle(newString);

    // Update fullscreen label to titlebar text as well
    ui->fullscreenLabel->setText(newString);
}

void MainWindow::updateWindowFilePath()
{
    if (!windowHandle())
        return;

    const bool shouldPopulate = getCurrentFileDetails().isPixmapLoaded;
    windowHandle()->setFilePath(shouldPopulate ? getCurrentFileDetails().fileInfo.absoluteFilePath()
                                               : "");
}

void MainWindow::setWindowSize()
{
    if (!getCurrentFileDetails().isPixmapLoaded)
        return;

    // check if the program is configured to resize the window
    int windowResizeMode =
            qvApp->getSettingsManager().getInt(SettingsManager::Setting::WindowResizeMode);
    if (!(windowResizeMode == 2 || (windowResizeMode == 1 && justLaunchedWithImage)))
        return;

    justLaunchedWithImage = false;

    // check if window is maximized or fullscreened
    if (windowState().testFlag(Qt::WindowMaximized) || windowState().testFlag(Qt::WindowFullScreen))
        return;

    qreal minWindowResizedPercentage =
            qvApp->getSettingsManager().getInt(SettingsManager::Setting::MinWindowResizedPercentage)
            / 100.0;
    qreal maxWindowResizedPercentage =
            qvApp->getSettingsManager().getInt(SettingsManager::Setting::MaxWindowResizedPercentage)
            / 100.0;

    QSize imageSize = getCurrentFileDetails().loadedPixmapSize;
    imageSize -= QSize(4, 4);

    // Try to grab the current screen
    QScreen *currentScreen = screenContaining(frameGeometry());

    // makeshift validity check
    bool screenValid = QGuiApplication::screens().contains(currentScreen);
    // Use first screen as fallback
    if (!screenValid)
        currentScreen = QGuiApplication::screens().at(0);

    // The pixmap signal runs before fileChanged, so side panels are not in the
    // layout yet. Place them first so the new window size includes them.
    updateThumbnailBarPosition();

    QSize extraWidgetsSize{ 0, 0 };

    if (menuBar()->isVisible())
        extraWidgetsSize.rheight() += menuBar()->height();

    if (thumbnailBar->isVisible()) {
        if (appliedThumbnailPosition == 2)
            extraWidgetsSize.rwidth() += thumbnailBar->width();
        else
            extraWidgetsSize.rheight() += thumbnailBar->height();
    }

    if (fileInfoPanel->isVisible()) {
        if (appliedInfoPosition == 1)
            extraWidgetsSize.rwidth() += fileInfoPanel->width();
        else
            extraWidgetsSize.rheight() += fileInfoPanel->height();
    }

    if (!windowState().testFlag(Qt::WindowFullScreen))
        extraWidgetsSize.rheight() += imageToolbar->sizeHint().height();

    const int titlebarOverlap = getTitlebarOverlap();
    if (titlebarOverlap != 0)
        extraWidgetsSize.rheight() += titlebarOverlap;

    const QSize windowFrameSize = frameGeometry().size() - geometry().size();
    const QSize hardLimitSize = currentScreen->availableSize() - windowFrameSize - extraWidgetsSize;
    const QSize screenSize = currentScreen->size();
    const QSize minWindowSize = (screenSize * minWindowResizedPercentage).boundedTo(hardLimitSize);
    const QSize maxWindowSize = (screenSize * maxWindowResizedPercentage).boundedTo(hardLimitSize);

    if (imageSize.width() < minWindowSize.width() && imageSize.height() < minWindowSize.height()) {
        imageSize.scale(minWindowSize, Qt::KeepAspectRatio);
    } else if (imageSize.width() > maxWindowSize.width()
               || imageSize.height() > maxWindowSize.height()) {
        imageSize.scale(maxWindowSize, Qt::KeepAspectRatio);
    }

    // Windows reports the wrong minimum width, so we constrain the image size relative to the dpi
    // to stop weirdness with tiny images
#ifdef Q_OS_WIN
    auto minimumImageSize = QSize(qRound(logicalDpiX() * 1.5), logicalDpiY() / 2);
    if (imageSize.boundedTo(minimumImageSize) == imageSize)
        imageSize = minimumImageSize;
#endif

    // Match center after new geometry
    // This is smoother than a single geometry set for some reason
    QRect oldRect = geometry();
    resize(imageSize + extraWidgetsSize);
    QRect newRect = geometry();
    newRect.moveCenter(oldRect.center());

    // Ensure titlebar is not above or below the available screen area
    const QRect availableScreenRect = currentScreen->availableGeometry();
    const int topFrameHeight = geometry().top() - frameGeometry().top();
    const int windowMinY = availableScreenRect.top() + topFrameHeight;
    const int windowMaxY =
            availableScreenRect.top() + availableScreenRect.height() - titlebarOverlap;
    if (newRect.top() < windowMinY)
        newRect.moveTop(windowMinY);
    if (newRect.top() > windowMaxY)
        newRect.moveTop(windowMaxY);

    setGeometry(newRect);
}

// Initially copied from Qt source code (QGuiApplication::screenAt) and then customized
QScreen *MainWindow::screenContaining(const QRect &rect)
{
    QScreen *bestScreen = nullptr;
    int bestScreenArea = 0;
    QVarLengthArray<const QScreen *, 8> visitedScreens;
    const auto screens = QGuiApplication::screens();
    for (const QScreen *screen : screens) {
        if (visitedScreens.contains(screen))
            continue;
        // The virtual siblings include the screen itself, so iterate directly
        const auto siblings = screen->virtualSiblings();
        for (QScreen *sibling : siblings) {
            const QRect intersect = sibling->geometry().intersected(rect);
            const int area = intersect.width() * intersect.height();
            if (area > bestScreenArea) {
                bestScreen = sibling;
                bestScreenArea = area;
            }
            visitedScreens.append(sibling);
        }
    }
    return bestScreen;
}

bool MainWindow::getIsPixmapLoaded() const
{
    return getCurrentFileDetails().isPixmapLoaded;
}

void MainWindow::setJustLaunchedWithImage(bool value)
{
    justLaunchedWithImage = value;
}

void MainWindow::openUrl(const QUrl &url)
{
    if (!url.isValid()) {
        QMessageBox::critical(this, tr("Error"), tr("Error: URL is invalid"));
        return;
    }

    auto request = QNetworkRequest(url);
    auto *reply = networkAccessManager.get(request);
    auto *progressDialog = new QProgressDialog(tr("Downloading image..."), tr("Cancel"), 0, 100);
    progressDialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    progressDialog->setAutoClose(false);
    progressDialog->setAutoReset(false);
    progressDialog->setWindowTitle(tr("Open URL..."));
    progressDialog->open();

    connect(progressDialog, &QProgressDialog::canceled, reply, [reply] { reply->abort(); });

    connect(reply, &QNetworkReply::downloadProgress, progressDialog,
            [progressDialog](qreal bytesReceived, qreal bytesTotal) {
                auto percent = (bytesReceived / bytesTotal) * 100;
                progressDialog->setValue(qRound(percent));
            });

    connect(reply, &QNetworkReply::finished, progressDialog, [progressDialog, reply, this] {
        if (reply->error()) {
            progressDialog->close();
            QMessageBox::critical(this, tr("Error"),
                                  tr("Error ") + QString::number(reply->error()) + ": "
                                          + reply->errorString());

            progressDialog->deleteLater();
            return;
        }

        progressDialog->setMaximum(0);

        auto *tempFile = new QTemporaryFile(this);
        tempFile->setFileTemplate(QDir::tempPath() + "/" + qvApp->applicationName()
                                  + ".XXXXXX.png");

        auto *saveFutureWatcher = new QFutureWatcher<bool>();
        connect(saveFutureWatcher, &QFutureWatcher<bool>::finished, this,
                [progressDialog, tempFile, saveFutureWatcher, this]() {
                    progressDialog->close();
                    if (saveFutureWatcher->result()) {
                        if (tempFile->open()) {
                            openFile(tempFile->fileName());
                        }
                    } else {
                        QMessageBox::critical(this, tr("Error"), tr("Error: Invalid image"));
                        tempFile->deleteLater();
                    }
                    progressDialog->deleteLater();
                    saveFutureWatcher->deleteLater();
                });

        saveFutureWatcher->setFuture(QtConcurrent::run([reply, tempFile] {
            return QImage::fromData(reply->readAll()).save(tempFile, "png");
        }));
    });
}

void MainWindow::pickUrl()
{
    auto inputDialog = new QInputDialog(this);
    inputDialog->setWindowTitle(tr("Open URL..."));
    inputDialog->setLabelText(tr("URL of a supported image file:"));
    inputDialog->resize(350, inputDialog->height());
    inputDialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    connect(inputDialog, &QInputDialog::finished, this, [inputDialog, this](int result) {
        if (result) {
            const auto url = QUrl(inputDialog->textValue());
            openUrl(url);
        }
        inputDialog->deleteLater();
    });
    inputDialog->open();
}

void MainWindow::reloadFile()
{
    graphicsView->reloadFile();
}

void MainWindow::openWith(const OpenWith::OpenWithItem &openWithItem)
{
    OpenWith::openWith(getCurrentFileDetails().fileInfo.absoluteFilePath(), openWithItem);
}

void MainWindow::openContainingFolder()
{
    if (!getCurrentFileDetails().isPixmapLoaded)
        return;

    const QFileInfo selectedFileInfo = getCurrentFileDetails().fileInfo;

#ifdef Q_OS_WIN
    QProcess::startDetached(
            "explorer",
            QStringList() << "/select,"
                          << QDir::toNativeSeparators(selectedFileInfo.absoluteFilePath()));
#elif defined Q_OS_MACOS
    QProcess::execute("open", QStringList() << "-R" << selectedFileInfo.absoluteFilePath());
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(selectedFileInfo.absolutePath()));
#endif
}

void MainWindow::showFileInfo()
{
    updateFileInfo(true, qvGetSettingInt(FileInfoPanelPosition) != 0);
    info->show();
    info->raise();
}

void MainWindow::askDeleteFile(bool permanent, bool showNext)
{
    if (!permanent && !qvApp->getSettingsManager().getBool(SettingsManager::Setting::AskDelete)) {
        deleteFile(permanent, showNext);
        return;
    }

    const QFileInfo &fileInfo = getCurrentFileDetails().fileInfo;
    const QString fileName = getCurrentFileDetails().fileInfo.fileName();

    if (!fileInfo.isWritable()) {
        QMessageBox::critical(
                this, tr("Error"),
                tr("Can't delete %1:\nNo write permission or file is read-only.").arg(fileName));
        return;
    }

    QString messageText;
    if (permanent) {
        messageText = tr("Are you sure you want to delete %1 permanently? This can't be undone.")
                              .arg(fileName);
    } else {
#ifdef Q_OS_WIN
        messageText = tr("Are you sure you want to move %1 to the Recycle Bin?").arg(fileName);
#else
        messageText = tr("Are you sure you want to move %1 to the Trash?").arg(fileName);
#endif
    }

    auto *msgBox = new QMessageBox(QMessageBox::Question, tr("Delete"), messageText,
                                   QMessageBox::Yes | QMessageBox::No, this);
    if (!permanent)
        msgBox->setCheckBox(new QCheckBox(tr("Do not ask again")));

    connect(msgBox, &QMessageBox::finished, this, [this, msgBox, permanent, showNext](int result) {
        if (result != QMessageBox::Yes)
            return;

        if (!permanent) {
            QSettings settings;
            settings.beginGroup("options");
            settings.setValue("askdelete", !msgBox->checkBox()->isChecked());
            qvApp->getSettingsManager().loadSettings();
        }
        this->deleteFile(permanent, showNext);
    });

    msgBox->open();
}

void MainWindow::deleteFile(bool permanent, bool showNext)
{
    const QFileInfo &fileInfo = getCurrentFileDetails().fileInfo;
    const QString filePath = fileInfo.absoluteFilePath();
    const QString fileName = fileInfo.fileName();

    graphicsView->closeImage();

    bool success;
    QString trashFilePath;
    if (permanent) {
        success = QFile::remove(filePath);
    } else {
#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
        QFile file(filePath);
        success = file.moveToTrash();
        if (success)
            trashFilePath = file.fileName();
#elif defined Q_OS_MACOS && COCOA_LOADED
        QString trashedFile = QVCocoaFunctions::deleteFile(filePath);
        success = !trashedFile.isEmpty();
        if (success)
            trashFilePath = QUrl(trashedFile).toLocalFile(); // remove file:// protocol
#elif defined Q_OS_UNIX && !defined Q_OS_MACOS
        trashFilePath = deleteFileLinuxFallback(filePath, false);
        success = !trashFilePath.isEmpty();
#else
        QMessageBox::critical(this, tr("Not Supported"),
                              tr("This program was compiled with an old version of Qt and this "
                                 "feature is not available.\n"
                                 "If you see this message, please report a bug!"));

        return;
#endif
    }

    if (!success || QFile::exists(filePath)) {
        openFile(filePath);
        QMessageBox::critical(this, tr("Error"), tr("Can't delete %1.").arg(fileName));
        return;
    }

    auto afterDelete = qvApp->getSettingsManager().getInt(SettingsManager::Setting::AfterDelete);
    if (showNext || afterDelete > 1)
        nextFile();
    else if (afterDelete < 1)
        previousFile();

    // Toolbar delete always continues with another image. The last file in a folder has no
    // next image, so show the previous one instead of leaving the window empty.
    if (showNext && !getCurrentFileDetails().isPixmapLoaded)
        previousFile();

    if (!trashFilePath.isEmpty())
        lastDeletedFiles.push({ trashFilePath, filePath });

    disableActions();
}

QString MainWindow::deleteFileLinuxFallback(const QString &path, bool putBack)
{
    QStringList gioArgs = { "trash", path };
    if (putBack)
        gioArgs.insert(1, "--restore");

    QProcess process;
    process.start("gio", gioArgs);
    process.waitForFinished();

    if (process.error() != QProcess::FailedToStart && !putBack) {
        process.start("gio", { "trash", "--list" });
        process.waitForFinished();

        const auto &output = QString(process.readAllStandardOutput()).split("\n");
        for (const auto &line : output) {
            if (line.contains(path))
                return line.split("\t").at(0);
        }
    }

    qWarning("Failed to use linux fallback delete");
    return "";
}

void MainWindow::undoDelete()
{
    if (lastDeletedFiles.isEmpty())
        return;

    const DeletedPaths lastDeletedFile = lastDeletedFiles.pop();
    if (lastDeletedFile.pathInTrash.isEmpty() || lastDeletedFile.previousPath.isEmpty())
        return;

#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)) || (defined Q_OS_MACOS && COCOA_LOADED)
    const QFileInfo fileInfo(lastDeletedFile.pathInTrash);
    if (!fileInfo.isWritable()) {
        QMessageBox::critical(this, tr("Error"),
                              tr("Can't undo deletion of %1:\n"
                                 "No write permission or file is read-only.")
                                      .arg(fileInfo.fileName()));
        return;
    }

    bool success = QFile::rename(lastDeletedFile.pathInTrash, lastDeletedFile.previousPath);
    if (!success) {
        QMessageBox::critical(this, tr("Error"),
                              tr("Failed undoing deletion of %1.").arg(fileInfo.fileName()));
    }
#elif defined Q_OS_UNIX && !defined Q_OS_MACOS
    deleteFileLinuxFallback(lastDeletedFile.pathInTrash, true);
#else
    QMessageBox::critical(this, tr("Not Supported"),
                          tr("This program was compiled with an old version of Qt and this feature "
                             "is not available.\n"
                             "If you see this message, please report a bug!"));

    return;
#endif

    openFile(lastDeletedFile.previousPath);
    disableActions();
}

void MainWindow::copy()
{
    auto *mimeData = graphicsView->getMimeData();
    if (!mimeData->hasImage() || !mimeData->hasUrls()) {
        mimeData->deleteLater();
        return;
    }

    QApplication::clipboard()->setMimeData(mimeData);
}

void MainWindow::paste()
{
    const QMimeData *mimeData = QApplication::clipboard()->mimeData();
    if (mimeData == nullptr)
        return;

    if (mimeData->hasText()) {
        auto url = QUrl(mimeData->text());

        if (url.isValid() && (url.scheme() == "http" || url.scheme() == "https")) {
            openUrl(url);
            return;
        }
    }

    graphicsView->loadMimeData(mimeData);
}

void MainWindow::rename()
{
    if (!getCurrentFileDetails().isPixmapLoaded)
        return;

    auto *renameDialog = new QVRenameDialog(this, getCurrentFileDetails().fileInfo);
    connect(renameDialog, &QVRenameDialog::newFileToOpen, this, &MainWindow::openFile);
    connect(renameDialog, &QVRenameDialog::readyToRenameFile, this, [this]() {
        if (auto device = graphicsView->getLoadedMovie().device()) {
            device->close();
        }
    });

    renameDialog->open();
}

void MainWindow::zoomIn()
{
    graphicsView->zoomIn();
}

void MainWindow::zoomOut()
{
    graphicsView->zoomOut();
}

void MainWindow::resetZoom()
{
    graphicsView->resetScale();
}

void MainWindow::originalSize()
{
    graphicsView->originalSize();
}

void MainWindow::rotateRight()
{
    graphicsView->rotateImage(90);
    resetZoom();
}

void MainWindow::rotateLeft()
{
    graphicsView->rotateImage(-90);
    resetZoom();
}

void MainWindow::mirror()
{
    if (getCurrentFileDetails().isMovieLoaded) {
        graphicsView->scale(-1, 1);
        resetZoom();
        return;
    }
    graphicsView->mirrorImage();
}

void MainWindow::flip()
{
    if (getCurrentFileDetails().isMovieLoaded) {
        graphicsView->scale(1, -1);
        resetZoom();
        return;
    }
    graphicsView->flipImage();
}

void MainWindow::saveImage()
{
    if (!graphicsView->isImageModified())
        return;

    const QString error = graphicsView->saveOverOriginal();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, tr("Save"),
                             tr("Could not overwrite the original image.\n%1").arg(error));
        return;
    }

    updateImageToolbar();
    updateWindowTitle();
    refreshProperties();
    thumbnailBar->refreshThumbnails();
}

void MainWindow::revertImage()
{
    if (!graphicsView->isImageModified())
        return;

    graphicsView->revertEdits();
}

void MainWindow::cropImage()
{
    if (graphicsView->isCropping())
        graphicsView->cancelCrop();
    else if (!graphicsView->beginCrop())
        cropButton->setChecked(false);
}

void MainWindow::compressImage()
{
    if (!graphicsView->canEditStill())
        return;

    const QImage sourceImage = graphicsView->getLoadedPixmap().toImage();
    const QSize sourceSize = sourceImage.size();
    const qint64 originalBytes = getCurrentFileDetails().fileInfo.size();
    const QByteArray format = graphicsView->originalFormat();
    const int longestEdge = qMax(sourceSize.width(), sourceSize.height());

    auto *dialog = new QDialog(this);
    dialog->setWindowTitle(tr("Compress"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(dialog);

    layout->addWidget(new QLabel(tr("Compressed preview"), dialog));
    auto *preview = new QLabel(tr("Calculating…"), dialog);
    preview->setAlignment(Qt::AlignCenter);
    preview->setFixedSize(560, 320);
    preview->setStyleSheet(QStringLiteral(
            "QLabel { background: rgba(127, 127, 127, 40); border-radius: 6px; }"));
    layout->addWidget(preview, 0, Qt::AlignHCenter);

    auto *summaryLabel = new QLabel(tr("Calculating…"), dialog);
    summaryLabel->setAlignment(Qt::AlignCenter);
    summaryLabel->setWordWrap(true);
    layout->addWidget(summaryLabel);

    auto *form = new QFormLayout();
    auto *qualitySlider = new QSlider(Qt::Horizontal, dialog);
    qualitySlider->setRange(1, 100);
    qualitySlider->setValue(85);
    auto *qualityValue = new QLabel(QString::number(qualitySlider->value()), dialog);
    auto *qualityRow = new QHBoxLayout();
    qualityRow->addWidget(qualitySlider, 1);
    qualityRow->addWidget(qualityValue);
    form->addRow(tr("Quality"), qualityRow);
    connect(qualitySlider, qOverload<int>(&QSlider::valueChanged), qualityValue,
            [qualityValue](int value) { qualityValue->setNum(value); });

    auto *sizeMode = new QComboBox(dialog);
    sizeMode->addItem(tr("Percent"));
    sizeMode->addItem(tr("Longest edge"));
    form->addRow(tr("Size"), sizeMode);

    auto *sizeValue = new QSpinBox(dialog);
    sizeValue->setRange(1, 100);
    sizeValue->setValue(100);
    sizeValue->setSuffix(QStringLiteral("%"));
    form->addRow(QString(), sizeValue);
    layout->addLayout(form);

    auto *note = new QLabel(tr("The original file is overwritten only when you press Save."), dialog);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    auto *applyButton = buttons->button(QDialogButtonBox::Ok);
    if (applyButton) {
        applyButton->setText(tr("Apply"));
        applyButton->setEnabled(false);
    }
    layout->addWidget(buttons);

    const auto targetSize = [=]() {
        QSize target = sourceSize;
        if (sizeMode->currentIndex() == 0) {
            target.setWidth(qMax(1, qRound(sourceSize.width() * sizeValue->value() / 100.0)));
            target.setHeight(qMax(1, qRound(sourceSize.height() * sizeValue->value() / 100.0)));
        } else if (longestEdge > 0) {
            target.scale(sizeValue->value(), sizeValue->value(), Qt::KeepAspectRatio);
            target.setWidth(qMax(1, target.width()));
            target.setHeight(qMax(1, target.height()));
        }
        return target;
    };

    auto *debounce = new QTimer(dialog);
    debounce->setSingleShot(true);
    debounce->setInterval(300);
    auto *watcher = new QFutureWatcher<QVImageCore::CompressionOutput>(dialog);
    auto busy = std::make_shared<bool>(false);
    auto dirty = std::make_shared<bool>(false);
    auto latest = std::make_shared<QVImageCore::CompressionOutput>();
    auto latestTarget = std::make_shared<QSize>();
    auto latestQuality = std::make_shared<int>(-1);

    const auto launch = [=]() {
        if (*busy) {
            *dirty = true;
            return;
        }
        *busy = true;
        *dirty = false;
        if (applyButton)
            applyButton->setEnabled(false);
        summaryLabel->setText(tr("Calculating…"));
        const QSize target = targetSize();
        const int quality = qualitySlider->value();
        watcher->setFuture(QtConcurrent::run([sourceImage, target, format, quality]() {
            return QVImageCore::encodeCompressed(sourceImage, target, format, quality);
        }));
    };

    connect(watcher, &QFutureWatcher<QVImageCore::CompressionOutput>::finished, dialog, [=]() {
        *busy = false;
        if (debounce->isActive() || *dirty) {
            if (!debounce->isActive())
                QTimer::singleShot(0, dialog, launch);
            return;
        }

        const QVImageCore::CompressionOutput output = watcher->result();
        if (output.encoded.isEmpty() || output.image.isNull()) {
            preview->setPixmap(QPixmap());
            preview->setText(tr("Could not compress this image."));
            summaryLabel->clear();
            if (applyButton)
                applyButton->setEnabled(false);
            return;
        }

        const qreal dpr = preview->devicePixelRatioF();
        QPixmap pix = QPixmap::fromImage(output.image);
        const QSize maxPx(qRound(preview->width() * dpr), qRound(preview->height() * dpr));
        if (pix.width() > maxPx.width() || pix.height() > maxPx.height())
            pix = pix.scaled(maxPx, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pix.setDevicePixelRatio(dpr);
        preview->setText(QString());
        preview->setPixmap(pix);
        *latest = output;
        *latestTarget = targetSize();
        *latestQuality = qualitySlider->value();
        summaryLabel->setText(tr("%1 x %2\n%3")
                                      .arg(output.image.width())
                                      .arg(output.image.height())
                                      .arg(compressionSummary(originalBytes, output.encoded.size())));
        if (applyButton)
            applyButton->setEnabled(true);
    });
    connect(debounce, &QTimer::timeout, dialog, launch);

    const auto schedule = [=]() {
        *dirty = true;
        if (applyButton)
            applyButton->setEnabled(false);
        debounce->start();
    };
    connect(qualitySlider, qOverload<int>(&QSlider::valueChanged), dialog, [=](int) { schedule(); });
    connect(sizeValue, qOverload<int>(&QSpinBox::valueChanged), dialog, [=](int) { schedule(); });
    connect(sizeMode, qOverload<int>(&QComboBox::currentIndexChanged), dialog, [=](int index) {
        if (index == 0) {
            sizeValue->setRange(1, 100);
            sizeValue->setSuffix(QStringLiteral("%"));
            sizeValue->setValue(100);
        } else {
            sizeValue->setRange(1, qMax(1, longestEdge));
            sizeValue->setSuffix(QStringLiteral(" px"));
            sizeValue->setValue(qMax(1, longestEdge));
        }
        schedule();
    });

    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::accepted, this, [=]() {
        const QSize target = targetSize();
        const int quality = qualitySlider->value();
        const bool reuse = !latest->encoded.isEmpty() && *latestTarget == target
                && *latestQuality == quality;
        const bool applied = reuse ? graphicsView->applyCompression(*latest, quality)
                                   : graphicsView->compressImage(target, quality);
        if (!applied)
            QMessageBox::warning(this, tr("Compress"), tr("Could not compress this image."));
    });

    schedule();
    dialog->open();
}

void MainWindow::batchCompress()
{
    QStringList files;
    const QVImageCore::FileDetails &details = getCurrentFileDetails();
    files.reserve(details.folderFileInfoList.size());
    for (const QVImageCore::CompatibleFile &file : details.folderFileInfoList)
        files.append(file.absoluteFilePath);

    int longestEdge = 0;
    if (graphicsView->canEditStill()) {
        const QSize size = graphicsView->getLoadedPixmap().size();
        longestEdge = qMax(size.width(), size.height());
    }

    auto *dialog = new QVBatchCompressDialog(files, details.fileInfo.absoluteFilePath(),
                                             graphicsView->isImageModified(), longestEdge, this);
    connect(dialog, &QVBatchCompressDialog::originalsOverwritten, this,
            [this](const QStringList &paths) {
                if (paths.isEmpty())
                    return;
                thumbnailBar->refreshThumbnails();
                if (paths.contains(getCurrentFileDetails().fileInfo.absoluteFilePath()))
                    reloadFile();
            });
    dialog->open();
}

void MainWindow::firstFile()
{
    graphicsView->goToFile(QVGraphicsView::GoToFileMode::first);
}

void MainWindow::previousFile()
{
    graphicsView->goToFile(QVGraphicsView::GoToFileMode::previous);
}

void MainWindow::nextFile()
{
    graphicsView->goToFile(QVGraphicsView::GoToFileMode::next);
}

void MainWindow::lastFile()
{
    graphicsView->goToFile(QVGraphicsView::GoToFileMode::last);
}

void MainWindow::saveFrameAs()
{
    QSettings settings;
    settings.beginGroup("recents");
    if (!getCurrentFileDetails().isMovieLoaded)
        return;

    if (graphicsView->getLoadedMovie().state() == QMovie::Running) {
        pause();
    }
    QFileDialog *saveDialog = new QFileDialog(this, tr("Save Frame As..."));
    saveDialog->setDirectory(settings.value("lastFileDialogDir", QDir::homePath()).toString());
    saveDialog->setNameFilters(qvApp->getNameFilterList());
    saveDialog->selectFile(getCurrentFileDetails().fileInfo.baseName() + "-"
                           + QString::number(graphicsView->getLoadedMovie().currentFrameNumber())
                           + ".png");
    saveDialog->setDefaultSuffix("png");
    saveDialog->setAcceptMode(QFileDialog::AcceptSave);
    saveDialog->open();
    connect(saveDialog, &QFileDialog::fileSelected, this, [=](const QString &fileName) {
        graphicsView->originalSize();
        for (int i = 0; i < graphicsView->getLoadedMovie().frameCount(); i++)
            nextFrame();

        graphicsView->getLoadedMovie().currentPixmap().save(fileName, nullptr, 100);
        graphicsView->resetScale();
    });
}

void MainWindow::pause()
{
    if (!getCurrentFileDetails().isMovieLoaded)
        return;

    const auto pauseActions = qvApp->getActionManager().getAllClonesOfAction("pause", this);

    if (graphicsView->getLoadedMovie().state() == QMovie::Running) {
        graphicsView->setPaused(true);
        for (const auto &pauseAction : pauseActions) {
            pauseAction->setText(tr("Res&ume"));
            pauseAction->setIcon(QIcon::fromTheme("media-playback-start"));
        }
    } else {
        graphicsView->setPaused(false);
        for (const auto &pauseAction : pauseActions) {
            pauseAction->setText(tr("Pause"));
            pauseAction->setIcon(QIcon::fromTheme("media-playback-pause"));
        }
    }
}

void MainWindow::nextFrame()
{
    if (!getCurrentFileDetails().isMovieLoaded)
        return;

    graphicsView->jumpToNextFrame();
}

void MainWindow::toggleSlideshow()
{
    const auto slideshowActions = qvApp->getActionManager().getAllClonesOfAction("slideshow", this);

    if (slideshowTimer->isActive()) {
        slideshowTimer->stop();
        for (const auto &slideshowAction : slideshowActions) {
            slideshowAction->setText(tr("Start S&lideshow"));
            slideshowAction->setIcon(QIcon::fromTheme("media-playback-start"));
        }
    } else {
        slideshowTimer->start();
        for (const auto &slideshowAction : slideshowActions) {
            slideshowAction->setText(tr("Stop S&lideshow"));
            slideshowAction->setIcon(QIcon::fromTheme("media-playback-stop"));
        }
    }
}

void MainWindow::cancelSlideshow()
{
    if (slideshowTimer->isActive())
        toggleSlideshow();
}

void MainWindow::slideshowAction()
{
    if (qvApp->getSettingsManager().getBool(SettingsManager::Setting::SlideshowReversed))
        previousFile();
    else
        nextFile();
}

void MainWindow::decreaseSpeed()
{
    if (!getCurrentFileDetails().isMovieLoaded)
        return;

    graphicsView->setSpeed(graphicsView->getLoadedMovie().speed() - 25);
}

void MainWindow::resetSpeed()
{
    if (!getCurrentFileDetails().isMovieLoaded)
        return;

    graphicsView->setSpeed(100);
}

void MainWindow::increaseSpeed()
{
    if (!getCurrentFileDetails().isMovieLoaded)
        return;

    graphicsView->setSpeed(graphicsView->getLoadedMovie().speed() + 25);
}

void MainWindow::toggleFullScreen()
{
    // Note: This is only triggered by the menu action, so the logic here should be kept to a
    // minimum. Anything that needs to run even if the window manager initiated the change should be
    // triggered by QEvent::WindowStateChange.

    // Disable updates during window state change to resolve visual glitches on macOS if the
    // titlebar is hidden
    setUpdatesEnabled(false);

    if (windowState().testFlag(Qt::WindowFullScreen)) {
        setWindowState(storedWindowState);
    } else {
        storedWindowState = windowState();

        showFullScreen();
    }

    setUpdatesEnabled(true);
}

int MainWindow::getTitlebarOverlap() const
{
#ifdef COCOA_LOADED
    // To account for fullsizecontentview on mac
    return QVCocoaFunctions::getObscuredHeight(window()->windowHandle());
#endif

    return 0;
}

void MainWindow::toggleThumbnailBar()
{
    // Cycle: hidden -> bottom -> left -> hidden
    const int nextPosition = (qvGetSettingInt(ThumbnailBarPosition) + 1) % 3;

    QSettings settings;
    settings.beginGroup("options");
    settings.setValue("thumbnailbarposition", nextPosition);
    qvApp->getSettingsManager().loadSettings();
}

void MainWindow::toggleFileInfoPanel()
{
    // Cycle: hidden -> right -> bottom -> hidden
    const int nextPosition = (qvGetSettingInt(FileInfoPanelPosition) + 1) % 3;

    QSettings settings;
    settings.beginGroup("options");
    settings.setValue("fileinfopanelposition", nextPosition);
    qvApp->getSettingsManager().loadSettings();
}

void MainWindow::toggleThumbnailBarVisible()
{
    toggleRememberedPosition("thumbnailbarposition", "thumbnailbarlastvisible",
                             qvGetSettingInt(ThumbnailBarPosition),
                             static_cast<int>(QVThumbnailBar::Position::Left));
}

void MainWindow::toggleFileInfoPanelVisible()
{
    toggleRememberedPosition("fileinfopanelposition", "fileinfopanellastvisible",
                             qvGetSettingInt(FileInfoPanelPosition),
                             static_cast<int>(QVFileInfoPanel::Position::Right));
}

void MainWindow::toggleRememberedPosition(const QString &positionKey, const QString &lastVisibleKey,
                                          int current, int fallbackVisible)
{
    QSettings settings;
    settings.beginGroup("options");
    int next = 0;
    if (current == 0) {
        next = settings.value(lastVisibleKey, fallbackVisible).toInt();
        if (next == 0)
            next = fallbackVisible;
    } else {
        settings.setValue(lastVisibleKey, current);
    }
    settings.setValue(positionKey, next);
    qvApp->getSettingsManager().loadSettings();
}

void MainWindow::rememberVisiblePanels()
{
    QSettings settings;
    settings.beginGroup("options");
    const int thumbnailPosition = qvGetSettingInt(ThumbnailBarPosition);
    const int infoPosition = qvGetSettingInt(FileInfoPanelPosition);
    if (thumbnailPosition != 0)
        settings.setValue("thumbnailbarlastvisible", thumbnailPosition);
    if (infoPosition != 0)
        settings.setValue("fileinfopanellastvisible", infoPosition);
}

QString MainWindow::actionToolTip(const QString &key)
{
    const QAction *action = qvApp->getActionManager().getAction(key);
    if (!action)
        return {};
    QString text = action->text();
    text.remove(QLatin1Char('&'));
    return text;
}

void MainWindow::setupImageToolbar()
{
    qvApp->ensureFontLoaded(QStringLiteral(":/fonts/MaterialIconsOutlined-Regular.otf"));

    imageColumn = new QWidget(this);
    imageColumn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    imageColumnLayout = new QVBoxLayout(imageColumn);
    imageColumnLayout->setSpacing(0);
    imageColumnLayout->setContentsMargins(0, 0, 0, 0);

    imageToolbar = new ImageToolbar(imageColumn);
    auto *toolbarLayout = new QHBoxLayout(imageToolbar);
    toolbarLayout->setSpacing(2);
    toolbarLayout->setContentsMargins(8, 0, 8, 0);

    const auto addButton = [this, toolbarLayout](const QString &tip, bool checkable) {
        auto *button = new QToolButton(imageToolbar);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolTip(tip);
        button->setCheckable(checkable);
        button->setIconSize(QSize(kImageToolbarIconSize, kImageToolbarIconSize));
        toolbarLayout->addWidget(button);
        return button;
    };

    openButton = addButton(actionToolTip(QStringLiteral("open")), false);
    restoreButton = addButton(tr("Restore"), false);
    saveButton = addButton(tr("Save"), false);
    compressResultLabel = new QLabel(imageToolbar);
    compressResultLabel->setVisible(false);
    compressResultLabel->setAlignment(Qt::AlignVCenter);
    toolbarLayout->addWidget(compressResultLabel);
    cropButton = addButton(tr("Crop"), true);
    compressButton = addButton(tr("Compress"), false);
    auto *compressMenu = new QMenu(compressButton);
    compressCurrentAction = compressMenu->addAction(tr("Compress this image"));
    auto *batchCompressAction = compressMenu->addAction(tr("Batch compress…"));
    compressButton->setMenu(compressMenu);
    compressButton->setPopupMode(QToolButton::InstantPopup);
    rotateRightButton = addButton(actionToolTip(QStringLiteral("rotateright")), false);
    thumbnailBarButton = addButton(actionToolTip(QStringLiteral("togglethumbnailbar")), true);
    fileInfoButton = addButton(actionToolTip(QStringLiteral("togglefileinfopanel")), true);
    trashButton = addButton(actionToolTip(QStringLiteral("delete")), false);
    toolbarLayout->addStretch(1);

    restoreButton->setObjectName(QStringLiteral("restoreImageButton"));
    restoreButton->setVisible(false);
    saveButton->setObjectName(QStringLiteral("saveImageButton"));
    saveButton->setVisible(false);
    cropButton->setEnabled(false);
    compressButton->setEnabled(true);

    connect(openButton, &QToolButton::clicked, this, [this]() { qvApp->pickFile(this); });
    connect(restoreButton, &QToolButton::clicked, this, &MainWindow::revertImage);
    connect(saveButton, &QToolButton::clicked, this, &MainWindow::saveImage);
    connect(cropButton, &QToolButton::clicked, this, &MainWindow::cropImage);
    connect(compressButton, &QToolButton::clicked, this, &MainWindow::compressImage);
    connect(compressCurrentAction, &QAction::triggered, this, &MainWindow::compressImage);
    connect(batchCompressAction, &QAction::triggered, this, &MainWindow::batchCompress);
    connect(rotateRightButton, &QToolButton::clicked, this, &MainWindow::rotateRight);
    connect(thumbnailBarButton, &QToolButton::clicked, this,
            &MainWindow::toggleThumbnailBarVisible);
    connect(fileInfoButton, &QToolButton::clicked, this, &MainWindow::toggleFileInfoPanelVisible);
    connect(trashButton, &QToolButton::clicked, this, [this]() { askDeleteFile(false, true); });

    imageToolbar->setStyleSheet(QStringLiteral(
            "QToolButton { border: none; border-radius: 5px; background: transparent; padding: 4px; }"
            "QToolButton:hover { background: rgba(128, 128, 128, 50); }"
            "QToolButton:pressed { background: rgba(128, 128, 128, 80); }"
            "QToolButton:checked { background: rgba(128, 128, 128, 90); }"
            "QToolButton:checked:hover { background: rgba(128, 128, 128, 120); }"
            "QToolButton#restoreImageButton { background: rgba(67, 160, 71, 70); }"
            "QToolButton#restoreImageButton:hover { background: rgba(67, 160, 71, 120); }"
            "QToolButton#restoreImageButton:pressed { background: rgba(67, 160, 71, 160); }"
            "QToolButton#saveImageButton { background: rgba(229, 57, 53, 70); }"
            "QToolButton#saveImageButton:hover { background: rgba(229, 57, 53, 120); }"
            "QToolButton#saveImageButton:pressed { background: rgba(229, 57, 53, 160); }"));

    imageColumnLayout->addWidget(imageToolbar);
    imageColumnLayout->addWidget(graphicsView, 1);
}

void MainWindow::updateImageToolbar()
{
    if (!imageToolbar)
        return;

    const bool fullscreen = windowState().testFlag(Qt::WindowFullScreen);
    imageToolbar->setVisible(!fullscreen);
    imageColumnLayout->setContentsMargins(0, fullscreen ? 0 : getTitlebarOverlap(), 0, 0);

    const qreal ratio = devicePixelRatioF();
    const auto icon = [ratio](ushort codePoint) {
        return qvApp->iconFromFont(QStringLiteral("Material Icons Outlined"), QChar(codePoint),
                                   kImageToolbarIconSize, ratio);
    };
    openButton->setIcon(icon(0xe2c8));
    restoreButton->setIcon(qvApp->iconFromFont(QStringLiteral("Material Icons Outlined"),
                                               QChar(0xe8b3), kImageToolbarIconSize, ratio,
                                               QColor(0x43, 0xA0, 0x47)));
    saveButton->setIcon(qvApp->iconFromFont(QStringLiteral("Material Icons Outlined"),
                                            QChar(0xe161), kImageToolbarIconSize, ratio,
                                            QColor(0xE5, 0x39, 0x35)));
    cropButton->setIcon(icon(0xe3be));
    compressButton->setIcon(icon(0xe94d));
    rotateRightButton->setIcon(icon(0xe41a));
    thumbnailBarButton->setIcon(icon(0xe413));
    fileInfoButton->setIcon(icon(0xe88e));
    trashButton->setIcon(icon(0xe872));

    thumbnailBarButton->setChecked(qvGetSettingInt(ThumbnailBarPosition) != 0);
    fileInfoButton->setChecked(qvGetSettingInt(FileInfoPanelPosition) != 0);

    const bool hasImage = getCurrentFileDetails().isPixmapLoaded;
    const bool canEdit = graphicsView->canEditStill();
    rotateRightButton->setEnabled(hasImage);
    trashButton->setEnabled(hasImage);
    cropButton->setEnabled(canEdit);
    compressButton->setEnabled(true);
    compressButton->setPopupMode(canEdit ? QToolButton::MenuButtonPopup : QToolButton::InstantPopup);
    if (compressCurrentAction)
        compressCurrentAction->setEnabled(canEdit);
    compressButton->setToolTip(canEdit ? tr("Compress") : tr("Batch compress…"));
    cropButton->setChecked(graphicsView->isCropping());
    cropButton->setToolTip(graphicsView->isCropping() ? tr("Drag to select the area to keep.")
                                                      : tr("Crop"));
    const bool modified = graphicsView->isImageModified();
    restoreButton->setVisible(modified);
    saveButton->setVisible(modified);
    const qint64 encodedBytes = graphicsView->pendingEncodedBytes();
    const bool showCompression = modified && encodedBytes >= 0;
    compressResultLabel->setVisible(showCompression);
    if (showCompression) {
        const QString summary =
                compressionSummary(getCurrentFileDetails().fileInfo.size(), encodedBytes);
        compressResultLabel->setText(summary);
        compressResultLabel->setToolTip(summary);
    }

    const QColor background = customBackgroundColor.isValid()
            ? customBackgroundColor
            : palette().color(QPalette::Window);
    QPalette toolbarPalette = imageToolbar->palette();
    toolbarPalette.setColor(QPalette::Window, background);
    imageToolbar->setPalette(toolbarPalette);
    imageToolbar->setAutoFillBackground(true);
}

void MainWindow::updateThumbnailBarPosition()
{
    const int thumbnailSetting = qvGetSettingInt(ThumbnailBarPosition);
    const int infoSetting = qvGetSettingInt(FileInfoPanelPosition);
    const bool isFullscreen = windowState().testFlag(Qt::WindowFullScreen);
    const int thumbnailPosition = thumbnailSetting != 0 && !isFullscreen
                    && !getCurrentFileDetails().folderFileInfoList.isEmpty()
            ? thumbnailSetting
            : 0;
    const int infoPosition =
            infoSetting != 0 && !isFullscreen && getCurrentFileDetails().fileInfo.isFile()
            ? infoSetting
            : 0;

    if (thumbnailPosition != appliedThumbnailPosition || infoPosition != appliedInfoPosition) {
        auto *mainLayout = static_cast<QVBoxLayout *>(centralWidget()->layout());

        if (imageRowLayout) {
            imageRowLayout->removeWidget(imageColumn);
            imageRowLayout->removeWidget(thumbnailBar);
            imageRowLayout->removeWidget(fileInfoPanel);
            mainLayout->removeItem(imageRowLayout);
            delete imageRowLayout;
            imageRowLayout = nullptr;
        }

        mainLayout->removeWidget(imageColumn);
        mainLayout->removeWidget(thumbnailBar);
        mainLayout->removeWidget(fileInfoPanel);

        const bool thumbnailOnLeft =
                thumbnailPosition == static_cast<int>(QVThumbnailBar::Position::Left);
        const bool infoOnRight =
                infoPosition == static_cast<int>(QVFileInfoPanel::Position::Right);

        if (thumbnailPosition != 0)
            thumbnailBar->setPosition(thumbnailPosition);
        if (infoPosition != 0)
            fileInfoPanel->setPosition(infoPosition);

        if (thumbnailOnLeft || infoOnRight) {
            imageRowLayout = new QHBoxLayout();
            imageRowLayout->setContentsMargins(0, 0, 0, 0);
            imageRowLayout->setSpacing(0);
            if (thumbnailOnLeft)
                imageRowLayout->addWidget(thumbnailBar);
            imageRowLayout->addWidget(imageColumn, 1);
            if (infoOnRight)
                imageRowLayout->addWidget(fileInfoPanel);
            mainLayout->insertLayout(1, imageRowLayout);
        } else {
            mainLayout->insertWidget(1, imageColumn);
        }

        if (infoPosition == static_cast<int>(QVFileInfoPanel::Position::Bottom))
            mainLayout->addWidget(fileInfoPanel);
        if (thumbnailPosition == static_cast<int>(QVThumbnailBar::Position::Bottom))
            mainLayout->addWidget(thumbnailBar);

        appliedThumbnailPosition = thumbnailPosition;
        appliedInfoPosition = infoPosition;
    }

    const int titlebarOverlap = getTitlebarOverlap();
    thumbnailBar->setTitlebarInset(
            thumbnailPosition == static_cast<int>(QVThumbnailBar::Position::Left) ? titlebarOverlap
                                                                                  : 0);
    fileInfoPanel->setTitlebarInset(
            infoPosition == static_cast<int>(QVFileInfoPanel::Position::Right) ? titlebarOverlap
                                                                               : 0);
    thumbnailBar->setVisible(thumbnailPosition != 0);
    fileInfoPanel->setVisible(infoPosition != 0);
    updateImageToolbar();
}
