#ifndef QVBATCHPRINTDIALOG_H
#define QVBATCHPRINTDIALOG_H

#include <QDialog>
#include <QHash>
#include <QImage>
#include <QPageLayout>
#include <QRectF>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>

class QCloseEvent;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QListWidget;
class QPagedPaintDevice;
class QPainter;
class QProgressBar;
class QPushButton;
class QVPrintPagePreview;

class QVBatchPrintDialog : public QDialog
{
    Q_OBJECT

public:
    // folderFiles are the still images of the open folder. currentImage is the
    // live pixmap of currentFilePath, so unsaved edits (crop, fill, rotate)
    // print as they look on screen instead of their on disk version.
    QVBatchPrintDialog(const QStringList &folderFiles, const QString &currentFilePath,
                       const QImage &liveImage, QWidget *parent = nullptr);
    ~QVBatchPrintDialog() override;

    // Paper direction the job starts with: the chosen one, or the direction of
    // the first image when the dialog is left on adaptive.
    QPageLayout::Orientation chosenOrientation() const;

protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;

private:
    void addPath(const QString &path, bool checked);
    void addFiles();
    void setAllChecked(bool checked);
    int selectedCount() const;
    QStringList selectedPaths() const;
    QString previewPath() const;
    void updatePreview();
    void updateSelectionLabel();
    void updateOrientationNote();
    // Paper direction that fits one image: wider than tall prints landscape.
    QPageLayout::Orientation orientationFor(const QString &path) const;
    // Counts landscape images among the first paths. sampled reports how many
    // files were inspected, known how many of those could be measured.
    int landscapeCount(const QStringList &paths, int *known, int *sampled) const;
    QSize imageSizeFor(const QString &path) const;
    QImage decodeForPrint(const QString &path, int edge) const;
    void storeOrientationChoice(int index);

    // Printing and "Save as PDF…" share one page loop. It runs from the event
    // loop, one image per iteration, so the dialog stays responsive and Cancel
    // can stop a long folder between pages.
    void startPrint();
    void startPdfExport();
    void handlePrimaryButton();
    void startJob(QPainter *painter, QPagedPaintDevice *device, const QRectF &pageRect,
                  int resolution);
    void printNextPage();
    void applyOrientationFor(const QString &path);
    QRectF currentPageRect() const;
    void requestCancel();
    void finishJob(bool deviceFailed);
    void setInputsEnabled(bool enabled);

    QListWidget *fileList = nullptr;
    QVPrintPagePreview *pagePreview = nullptr;
    QLabel *previewCaption = nullptr;
    QComboBox *orientationMode = nullptr;
    QLabel *orientationNote = nullptr;
    QLabel *selectionLabel = nullptr;
    QProgressBar *progressBar = nullptr;
    QLabel *statusLabel = nullptr;
    QPushButton *addButton = nullptr;
    QPushButton *allButton = nullptr;
    QPushButton *noneButton = nullptr;
    QPushButton *printButton = nullptr;
    QPushButton *pdfButton = nullptr;
    QDialogButtonBox *buttonBox = nullptr;

    QString currentPath;
    QImage currentImage;
    QSet<QString> knownPaths;
    mutable QHash<QString, QSize> sizeCache;

    // Last preview decoded, so changing the paper direction does not decode again.
    QString previewPathCache;
    QImage previewImage;

    // State of the job in flight.
    QStringList jobPaths;
    QPainter *jobPainter = nullptr;
    QPagedPaintDevice *jobDevice = nullptr;
    QRectF jobPageRect;
    int jobResolution = 0;
    int jobEdge = 0;
    int jobIndex = 0;
    int jobPrinted = 0;
    int jobRemaining = 0;
    QStringList jobFailed;
    QString jobDestination;
    bool jobFirstPage = true;
    bool jobToPdf = false;
    bool running = false;
    bool cancelRequested = false;
    bool closeAfterFinish = false;
};

#endif // QVBATCHPRINTDIALOG_H
