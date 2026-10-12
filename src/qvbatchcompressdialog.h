#ifndef QVBATCHCOMPRESSDIALOG_H
#define QVBATCHCOMPRESSDIALOG_H

#include <QByteArray>
#include <QDialog>
#include <QFutureWatcher>
#include <QImage>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>

class QCloseEvent;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QShowEvent;
class QSlider;
class QSpinBox;
class QThread;
class QTimer;
class BatchCompressWorker;

struct QVBatchEncoded
{
    QImage image;
    QByteArray encoded;
    qint64 before = 0;
    QString error;
    QString fileName;
};

struct QVBatchJobResult
{
    int succeeded = 0;
    int skipped = 0;
    int skippedAnimated = 0;
    int failed = 0;
    int remaining = 0;
    bool cancelled = false;
    qint64 before = 0;
    qint64 after = 0;
    QStringList overwritten;
    // "fileName\tcode" for the first few failures. code is a short token or an OS message.
    QStringList errors;
};

class QVBatchCompressDialog : public QDialog
{
    Q_OBJECT

public:
    // folderFiles are the still-or-movie images already in the open folder.
    // currentFileModified refers to currentFilePath.
    QVBatchCompressDialog(const QStringList &folderFiles, const QString &currentFilePath,
                          bool currentFileModified, int currentLongestEdge,
                          QWidget *parent = nullptr);
    ~QVBatchCompressDialog() override;

signals:
    void originalsOverwritten(const QStringList &paths);

protected:
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void reject() override;

private:
    void addPath(const QString &path, bool checked);
    void refreshItemLabels();
    void addFiles();
    void setAllChecked(bool checked);
    QStringList selectedPaths() const;
    QString previewPath() const;
    void updateSelectionLabel();
    void updateOutputNote();
    void schedulePreview();
    void launchPreview();
    void showPreview(const QVBatchEncoded &encoded);
    void startOrCancel();
    void reportProgress(int done, int total, const QString &fileName);
    void setInputsEnabled(bool enabled);
    void showResult(const QVBatchJobResult &result);
    QString describeError(const QString &code) const;

    QListWidget *fileList = nullptr;
    QLabel *selectionLabel = nullptr;
    QLabel *preview = nullptr;
    QLabel *previewCaption = nullptr;
    QLabel *previewSummary = nullptr;
    QSlider *qualitySlider = nullptr;
    QLabel *qualityValue = nullptr;
    QComboBox *sizeMode = nullptr;
    QSpinBox *sizeValue = nullptr;
    QComboBox *outputMode = nullptr;
    QLabel *outputNote = nullptr;
    QProgressBar *progressBar = nullptr;
    QLabel *statusLabel = nullptr;
    QPushButton *startButton = nullptr;
    QDialogButtonBox *buttonBox = nullptr;
    QTimer *previewDebounce = nullptr;
    QFutureWatcher<QVBatchEncoded> *previewWatcher = nullptr;

    QString currentPath;
    bool currentModified = false;
    int currentLongestEdge = 0;
    bool running = false;
    bool closeAfterFinish = false;
    bool lastOverwrite = false;
    bool previewReady = false;
    bool previewBusy = false;
    bool previewDirty = false;
    std::shared_ptr<std::atomic_bool> cancelFlag;
    QThread *workerThread = nullptr;
    BatchCompressWorker *worker = nullptr;
};

#endif // QVBATCHCOMPRESSDIALOG_H
