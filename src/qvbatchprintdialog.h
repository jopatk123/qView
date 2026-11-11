#ifndef QVBATCHPRINTDIALOG_H
#define QVBATCHPRINTDIALOG_H

#include <QDialog>
#include <QHash>
#include <QImage>
#include <QPageLayout>
#include <QSize>
#include <QString>
#include <QStringList>

class QCloseEvent;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;

class QVBatchPrintDialog : public QDialog
{
    Q_OBJECT

public:
    // folderFiles are the still images of the open folder. currentImage is the
    // live pixmap of currentFilePath, so unsaved edits (crop, fill, rotate)
    // print as they look on screen instead of their on disk version.
    QVBatchPrintDialog(const QStringList &folderFiles, const QString &currentFilePath,
                       const QImage &liveImage, QWidget *parent = nullptr);

    // Orientation the print job will use: the chosen one, or the majority
    // orientation of the selected images when the user left it on automatic.
    QPageLayout::Orientation chosenOrientation() const;

protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;

private:
    void addPath(const QString &path, bool checked);
    void addFiles();
    void setAllChecked(bool checked);
    QStringList selectedPaths() const;
    void updateSelectionLabel();
    void updateOrientationNote();
    QPageLayout::Orientation autoOrientation() const;
    int landscapeCount(const QStringList &paths, int *known) const;
    QSize imageSizeFor(const QString &path) const;
    QImage decodeForPrint(const QString &path, const QSize &box) const;
    void startPrint();
    void setInputsEnabled(bool enabled);

    QListWidget *fileList = nullptr;
    QComboBox *orientationMode = nullptr;
    QLabel *orientationNote = nullptr;
    QLabel *selectionLabel = nullptr;
    QProgressBar *progressBar = nullptr;
    QLabel *statusLabel = nullptr;
    QPushButton *printButton = nullptr;
    QDialogButtonBox *buttonBox = nullptr;

    QString currentPath;
    QImage currentImage;
    bool running = false;
    bool closeAfterFinish = false;
    mutable QHash<QString, QSize> sizeCache;
};

#endif // QVBATCHPRINTDIALOG_H
