#include "qvbatchcompressdialog.h"
#include "qvapplication.h"
#include "qvimagecore.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QSlider>
#include <QSpinBox>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <QtConcurrent/QtConcurrentRun>

#include <functional>

namespace {

const int kMaxFailureLines = 8;
const int kLongestEdgeLimit = 16384;

// macOS draws item-view checkboxes outside the row, so only the first one stays visible.
class FileCheckDelegate : public QStyledItemDelegate
{
public:
    explicit FileCheckDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const bool selected = opt.state & QStyle::State_Selected;
        const QRect checkRect = indicatorRect(opt);
        const auto state = static_cast<Qt::CheckState>(index.data(Qt::CheckStateRole).toInt());

        painter->save();
        painter->setClipRect(opt.rect);
        if (selected)
            painter->fillRect(opt.rect, opt.palette.highlight());
        else if (opt.state & QStyle::State_MouseOver)
            painter->fillRect(opt.rect, opt.palette.color(QPalette::Mid));

        const QColor accent(10, 132, 255);
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(state == Qt::Checked ? accent : QColor(160, 160, 160), 1.4));
        painter->setBrush(state == Qt::Checked ? QBrush(accent) : Qt::NoBrush);
        painter->drawRoundedRect(QRectF(checkRect).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        if (state == Qt::Checked) {
            QPen mark(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            painter->setPen(mark);
            const QPointF start(checkRect.left() + 3.5, checkRect.center().y() + 0.5);
            const QPointF mid(checkRect.left() + 6.5, checkRect.bottom() - 4.0);
            const QPointF end(checkRect.right() - 3.0, checkRect.top() + 4.5);
            painter->drawLine(start, mid);
            painter->drawLine(mid, end);
        }

        const QRect textRect(checkRect.right() + 8, opt.rect.top(),
                             qMax(0, opt.rect.right() - 8 - (checkRect.right() + 8)),
                             opt.rect.height());
        painter->setRenderHint(QPainter::Antialiasing, false);
        painter->setFont(opt.font);
        painter->setPen(selected ? opt.palette.color(QPalette::HighlightedText)
                                 : opt.palette.color(QPalette::Text));
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                          opt.fontMetrics.elidedText(opt.text, Qt::ElideMiddle, textRect.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        const QWidget *widget = option.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        const int box = style->pixelMetric(QStyle::PM_IndicatorHeight, nullptr, widget);
        hint.setHeight(qMax(hint.height(), box + 8));
        return hint;
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        if (event->type() == QEvent::MouseButtonRelease) {
            const auto *mouse = static_cast<const QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                QStyleOptionViewItem opt = option;
                initStyleOption(&opt, index);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                const QPoint pos = mouse->position().toPoint();
#else
                const QPoint pos = mouse->pos();
#endif
                if (indicatorRect(opt).contains(pos)) {
                    const auto state =
                            static_cast<Qt::CheckState>(index.data(Qt::CheckStateRole).toInt());
                    model->setData(index, state == Qt::Checked ? Qt::Unchecked : Qt::Checked,
                                   Qt::CheckStateRole);
                    return true;
                }
            }
            return false;
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

private:
    static QRect indicatorRect(const QStyleOptionViewItem &option)
    {
        const QWidget *widget = option.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        const int box = qMax(16, style->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, widget));
        return QRect(option.rect.left() + 8, option.rect.top() + (option.rect.height() - box) / 2,
                     box, box);
    }
};

QString formatByteCount(qint64 bytes)
{
    return QLocale().formattedDataSize(qMax<qint64>(0, bytes));
}

QString compressionSummary(qint64 before, qint64 after)
{
    const QString afterText = formatByteCount(after);
    if (before <= 0)
        return QVBatchCompressDialog::tr("Compressed size: %1").arg(afterText);

    const qint64 delta = before - after;
    if (delta == 0)
        return QVBatchCompressDialog::tr("%1 → %2, same size").arg(formatByteCount(before), afterText);

    const int percent = int((qAbs(delta) * 100 + before / 2) / before);
    const QString deltaText = formatByteCount(qAbs(delta));
    const QString beforeText = formatByteCount(before);
    if (delta > 0) {
        return QVBatchCompressDialog::tr("%1 → %2, reduced %3 (%4%%)")
                .arg(beforeText, afterText, deltaText, QString::number(percent));
    }
    return QVBatchCompressDialog::tr("%1 → %2, increased %3 (%4%%)")
            .arg(beforeText, afterText, deltaText, QString::number(percent));
}

QSize targetSizeFor(const QSize &source, int sizeMode, int sizeValue)
{
    QSize target = source;
    if (!target.isValid() || target.isEmpty())
        return QSize();

    if (sizeMode == 0) {
        target.setWidth(qMax(1, qRound(source.width() * (sizeValue / 100.0))));
        target.setHeight(qMax(1, qRound(source.height() * (sizeValue / 100.0))));
        return target;
    }

    const int longest = qMax(source.width(), source.height());
    if (longest > sizeValue && sizeValue > 0)
        target.scale(sizeValue, sizeValue, Qt::KeepAspectRatio);
    target.setWidth(qMax(1, target.width()));
    target.setHeight(qMax(1, target.height()));
    return target;
}

QString copyDestination(const QString &sourcePath)
{
    const QFileInfo info(sourcePath);
    return QDir(info.absolutePath()).filePath(QStringLiteral("compressed/") + info.fileName());
}

QVBatchEncoded encodeFile(const QString &path, int quality, int sizeMode, int sizeValue)
{
    QVBatchEncoded result;
    const QFileInfo info(path);
    result.fileName = info.fileName();
    result.before = info.size();
    if (!info.isFile()) {
        result.error = QStringLiteral("read");
        return result;
    }

    QImageReader reader;
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    reader.setFileName(path);
    const QByteArray format = reader.format();
    if (format.isEmpty()) {
        result.error = QStringLiteral("read");
        return result;
    }
    if (reader.imageCount() > 1) {
        result.error = QStringLiteral("animated");
        return result;
    }

    QImage image = reader.read();
    if (image.isNull()) {
        result.error = QStringLiteral("read");
        return result;
    }

    const QSize target = targetSizeFor(image.size(), sizeMode, sizeValue);
    const QVImageCore::CompressionOutput output =
            QVImageCore::encodeCompressed(image, target, format, quality);
    if (output.encoded.isEmpty() || output.image.isNull()) {
        result.error = QStringLiteral("encode");
        return result;
    }

    result.image = output.image;
    result.encoded = output.encoded;
    return result;
}

void rememberFailure(QVBatchJobResult *result, const QString &name, const QString &code)
{
    if (result->errors.size() < kMaxFailureLines)
        result->errors.append(name + QLatin1Char('\t') + code);
}

QString writeEncoded(const QString &sourcePath, const QByteArray &encoded, bool overwrite)
{
    const QString destination = overwrite ? sourcePath : copyDestination(sourcePath);
    const QFileInfo destInfo(destination);
    if (!QDir().mkpath(destInfo.absolutePath()))
        return QStringLiteral("mkdir");

    QSaveFile file(destination);
    if (!file.open(QIODevice::WriteOnly))
        return file.errorString().isEmpty() ? QStringLiteral("write") : file.errorString();
    if (file.write(encoded) != encoded.size()) {
        const QString error = file.errorString();
        file.cancelWriting();
        return error.isEmpty() ? QStringLiteral("write") : error;
    }
    if (!file.commit())
        return file.errorString().isEmpty() ? QStringLiteral("write") : file.errorString();
    return {};
}

} // namespace

class BatchCompressWorker
{
public:
    using Progress = std::function<void(int done, int total, const QString &fileName)>;

    BatchCompressWorker(QStringList paths, int quality, int sizeMode, int sizeValue, bool overwrite,
                        std::shared_ptr<std::atomic_bool> cancel, QThread *hostThread,
                        Progress progress)
        : paths(std::move(paths)), quality(quality), sizeMode(sizeMode), sizeValue(sizeValue),
          overwrite(overwrite), cancel(std::move(cancel)), hostThread(hostThread),
          progress(std::move(progress))
    {
    }

    void run()
    {
        const int total = paths.size();
        for (int i = 0; i < total; ++i) {
            if (cancel && cancel->load()) {
                result.cancelled = true;
                result.remaining = total - i;
                break;
            }

            const QString path = paths.at(i);
            const QString name = QFileInfo(path).fileName();
            report(i, total, name);

            const QVBatchEncoded encoded = encodeFile(path, quality, sizeMode, sizeValue);
            if (encoded.error == QLatin1String("animated")) {
                ++result.skipped;
                ++result.skippedAnimated;
                continue;
            }
            if (!encoded.error.isEmpty() || encoded.encoded.isEmpty()) {
                ++result.failed;
                rememberFailure(&result, name,
                                encoded.error.isEmpty() ? QStringLiteral("encode") : encoded.error);
                continue;
            }

            const QString writeError = writeEncoded(path, encoded.encoded, overwrite);
            if (!writeError.isEmpty()) {
                ++result.failed;
                rememberFailure(&result, name, writeError);
                continue;
            }

            ++result.succeeded;
            result.before += encoded.before;
            result.after += encoded.encoded.size();
            if (overwrite)
                result.overwritten.append(QFileInfo(path).absoluteFilePath());
        }

        if (!result.cancelled)
            report(total, total, QString());
        if (hostThread)
            hostThread->quit();
    }

    QVBatchJobResult result;

private:
    void report(int done, int total, const QString &name) const
    {
        if (progress)
            progress(done, total, name);
    }

    QStringList paths;
    int quality = 85;
    int sizeMode = 0;
    int sizeValue = 100;
    bool overwrite = false;
    std::shared_ptr<std::atomic_bool> cancel;
    QThread *hostThread = nullptr;
    Progress progress;
};

QVBatchCompressDialog::QVBatchCompressDialog(const QStringList &folderFiles,
                                             const QString &currentFilePath,
                                             bool currentFileModified, int currentLongestEdge,
                                             QWidget *parent)
    : QDialog(parent), currentPath(QFileInfo(currentFilePath).absoluteFilePath()),
      currentModified(currentFileModified), currentLongestEdge(currentLongestEdge)
{
    setWindowTitle(tr("Batch compress"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setWindowModality(Qt::WindowModal);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(680, 760);

    auto *layout = new QVBoxLayout(this);

    layout->addWidget(new QLabel(tr("Files"), this));
    fileList = new QListWidget(this);
    fileList->setItemDelegate(new FileCheckDelegate(fileList));
    fileList->setSelectionMode(QAbstractItemView::SingleSelection);
    fileList->setTextElideMode(Qt::ElideMiddle);
    fileList->setMinimumHeight(150);
    layout->addWidget(fileList, 1);

    auto *fileButtons = new QHBoxLayout();
    auto *addButton = new QPushButton(tr("Add files…"), this);
    auto *allButton = new QPushButton(tr("All"), this);
    auto *noneButton = new QPushButton(tr("None"), this);
    selectionLabel = new QLabel(this);
    fileButtons->addWidget(addButton);
    fileButtons->addWidget(allButton);
    fileButtons->addWidget(noneButton);
    fileButtons->addStretch(1);
    fileButtons->addWidget(selectionLabel);
    layout->addLayout(fileButtons);

    auto *hint = new QLabel(tr("Animated images are skipped."), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    layout->addWidget(new QLabel(tr("Preview"), this));
    preview = new QLabel(tr("Select an image to preview."), this);
    preview->setAlignment(Qt::AlignCenter);
    preview->setFixedSize(520, 220);
    preview->setStyleSheet(QStringLiteral(
            "QLabel { background: rgba(127, 127, 127, 40); border-radius: 6px; }"));
    layout->addWidget(preview, 0, Qt::AlignHCenter);
    previewCaption = new QLabel(this);
    previewCaption->setAlignment(Qt::AlignCenter);
    previewCaption->setWordWrap(true);
    layout->addWidget(previewCaption);
    previewSummary = new QLabel(this);
    previewSummary->setAlignment(Qt::AlignCenter);
    previewSummary->setWordWrap(true);
    layout->addWidget(previewSummary);

    auto *form = new QFormLayout();
    qualitySlider = new QSlider(Qt::Horizontal, this);
    qualitySlider->setRange(1, 100);
    qualitySlider->setValue(85);
    qualityValue = new QLabel(QString::number(qualitySlider->value()), this);
    auto *qualityRow = new QHBoxLayout();
    qualityRow->addWidget(qualitySlider, 1);
    qualityRow->addWidget(qualityValue);
    form->addRow(tr("Quality"), qualityRow);

    sizeMode = new QComboBox(this);
    sizeMode->addItem(tr("Percent"));
    sizeMode->addItem(tr("Longest edge"));
    sizeValue = new QSpinBox(this);
    sizeValue->setRange(1, 100);
    sizeValue->setValue(100);
    sizeValue->setSuffix(QStringLiteral("%"));
    auto *sizeRow = new QHBoxLayout();
    sizeRow->addWidget(sizeMode, 1);
    sizeRow->addWidget(sizeValue);
    form->addRow(tr("Size"), sizeRow);

    outputMode = new QComboBox(this);
    outputMode->addItem(tr("Save copies into a \"compressed\" folder"));
    outputMode->addItem(tr("Overwrite original files"));
    form->addRow(tr("Output"), outputMode);
    layout->addLayout(form);

    outputNote = new QLabel(this);
    outputNote->setWordWrap(true);
    layout->addWidget(outputNote);

    progressBar = new QProgressBar(this);
    progressBar->setTextVisible(false);
    progressBar->setVisible(false);
    layout->addWidget(progressBar);
    statusLabel = new QLabel(this);
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    startButton = buttonBox->addButton(tr("Start"), QDialogButtonBox::ActionRole);
    startButton->setDefault(true);
    layout->addWidget(buttonBox);

    previewDebounce = new QTimer(this);
    previewDebounce->setSingleShot(true);
    previewDebounce->setInterval(300);
    previewWatcher = new QFutureWatcher<QVBatchEncoded>(this);

    connect(qualitySlider, &QSlider::valueChanged, qualityValue,
            [this](int value) { qualityValue->setNum(value); });
    connect(qualitySlider, &QSlider::valueChanged, this, [this](int) { schedulePreview(); });
    connect(sizeValue, qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int) { schedulePreview(); });
    connect(sizeMode, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        const QSignalBlocker blocker(sizeValue);
        if (index == 0) {
            sizeValue->setRange(1, 100);
            sizeValue->setSuffix(QStringLiteral("%"));
            sizeValue->setValue(100);
        } else {
            sizeValue->setRange(1, kLongestEdgeLimit);
            sizeValue->setSuffix(QStringLiteral(" px"));
            const int edge = this->currentLongestEdge > 0 ? this->currentLongestEdge : 2560;
            sizeValue->setValue(qBound(1, edge, kLongestEdgeLimit));
        }
        schedulePreview();
    });
    connect(outputMode, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { updateOutputNote(); });
    connect(addButton, &QPushButton::clicked, this, &QVBatchCompressDialog::addFiles);
    connect(allButton, &QPushButton::clicked, this, [this]() { setAllChecked(true); });
    connect(noneButton, &QPushButton::clicked, this, [this]() { setAllChecked(false); });
    connect(fileList, &QListWidget::itemChanged, this, [this](QListWidgetItem *) {
        updateSelectionLabel();
        schedulePreview();
    });
    connect(fileList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *, QListWidgetItem *) { schedulePreview(); });
    connect(previewDebounce, &QTimer::timeout, this, &QVBatchCompressDialog::launchPreview);
    connect(previewWatcher, &QFutureWatcher<QVBatchEncoded>::finished, this, [this]() {
        previewBusy = false;
        if (running)
            return;
        if (previewDebounce->isActive() || previewDirty) {
            if (!previewDebounce->isActive())
                QTimer::singleShot(0, this, [this]() { launchPreview(); });
            return;
        }
        showPreview(previewWatcher->result());
    });
    connect(startButton, &QPushButton::clicked, this, &QVBatchCompressDialog::startOrCancel);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QVBatchCompressDialog::reject);

    for (const QString &path : folderFiles)
        addPath(path, true);
    refreshItemLabels();
    updateSelectionLabel();
    updateOutputNote();

    if (!currentPath.isEmpty()) {
        for (int i = 0; i < fileList->count(); ++i) {
            if (fileList->item(i)->data(Qt::UserRole).toString() == currentPath) {
                fileList->setCurrentRow(i);
                fileList->scrollToItem(fileList->item(i));
                break;
            }
        }
    }
    if (!fileList->currentItem() && fileList->count() > 0)
        fileList->setCurrentRow(0);
}

QVBatchCompressDialog::~QVBatchCompressDialog()
{
    if (cancelFlag)
        cancelFlag->store(true);
    if (workerThread && workerThread->isRunning())
        workerThread->wait();
    delete worker;
    worker = nullptr;
    workerThread = nullptr;
}

void QVBatchCompressDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (!previewReady) {
        previewReady = true;
        schedulePreview();
    }
}

void QVBatchCompressDialog::closeEvent(QCloseEvent *event)
{
    if (running) {
        if (cancelFlag)
            cancelFlag->store(true);
        closeAfterFinish = true;
        startButton->setEnabled(false);
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void QVBatchCompressDialog::reject()
{
    if (running) {
        if (cancelFlag)
            cancelFlag->store(true);
        closeAfterFinish = true;
        startButton->setEnabled(false);
        return;
    }
    QDialog::reject();
}

void QVBatchCompressDialog::addPath(const QString &path, bool checked)
{
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (absolute.isEmpty() || !QFileInfo(absolute).isFile())
        return;
    for (int i = 0; i < fileList->count(); ++i) {
        if (fileList->item(i)->data(Qt::UserRole).toString() == absolute)
            return;
    }

    auto *item = new QListWidgetItem(QFileInfo(absolute).fileName(), fileList);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    item->setData(Qt::UserRole, absolute);
    item->setToolTip(absolute);
}

void QVBatchCompressDialog::refreshItemLabels()
{
    QSet<QString> directories;
    for (int i = 0; i < fileList->count(); ++i)
        directories.insert(QFileInfo(fileList->item(i)->data(Qt::UserRole).toString()).absolutePath());
    const bool showParent = directories.size() > 1;

    const QSignalBlocker blocker(fileList);
    for (int i = 0; i < fileList->count(); ++i) {
        const QFileInfo info(fileList->item(i)->data(Qt::UserRole).toString());
        const QString text = showParent ? info.dir().dirName() + QLatin1Char('/') + info.fileName()
                                        : info.fileName();
        fileList->item(i)->setText(text);
    }
}

void QVBatchCompressDialog::addFiles()
{
    QSettings settings;
    settings.beginGroup("recents");
    QString directory = settings.value("lastFileDialogDir", QDir::homePath()).toString();
    if (!currentPath.isEmpty())
        directory = QFileInfo(currentPath).absolutePath();

    const QStringList picked = QFileDialog::getOpenFileNames(
            this, tr("Add files"), directory, qvApp->getNameFilterList().join(QStringLiteral(";;")));
    if (picked.isEmpty())
        return;

    settings.setValue("lastFileDialogDir", QFileInfo(picked.constFirst()).absolutePath());
    QListWidgetItem *firstNew = nullptr;
    const int previousCount = fileList->count();
    for (const QString &path : picked)
        addPath(path, true);
    refreshItemLabels();
    updateSelectionLabel();
    if (fileList->count() > previousCount)
        firstNew = fileList->item(previousCount);
    if (firstNew) {
        fileList->setCurrentItem(firstNew);
        fileList->scrollToItem(firstNew);
    }
    schedulePreview();
}

void QVBatchCompressDialog::setAllChecked(bool checked)
{
    const QSignalBlocker blocker(fileList);
    const auto state = checked ? Qt::Checked : Qt::Unchecked;
    for (int i = 0; i < fileList->count(); ++i)
        fileList->item(i)->setCheckState(state);
    updateSelectionLabel();
    schedulePreview();
}

QStringList QVBatchCompressDialog::selectedPaths() const
{
    QStringList paths;
    for (int i = 0; i < fileList->count(); ++i) {
        if (fileList->item(i)->checkState() == Qt::Checked)
            paths.append(fileList->item(i)->data(Qt::UserRole).toString());
    }
    return paths;
}

QString QVBatchCompressDialog::previewPath() const
{
    if (QListWidgetItem *item = fileList->currentItem())
        return item->data(Qt::UserRole).toString();
    const QStringList selected = selectedPaths();
    return selected.isEmpty() ? QString() : selected.constFirst();
}

void QVBatchCompressDialog::updateSelectionLabel()
{
    const int total = fileList->count();
    int selected = 0;
    for (int i = 0; i < total; ++i) {
        if (fileList->item(i)->checkState() == Qt::Checked)
            ++selected;
    }
    selectionLabel->setText(tr("%1 of %2 selected").arg(selected).arg(total));
    if (!running)
        startButton->setEnabled(selected > 0);
}

void QVBatchCompressDialog::updateOutputNote()
{
    if (outputMode->currentIndex() == 0) {
        outputNote->setText(tr(
                "Copies are written next to each original, in a folder named \"compressed\"."));
    } else {
        outputNote->setText(tr("Original files are replaced. This cannot be undone."));
    }
}

void QVBatchCompressDialog::schedulePreview()
{
    if (running)
        return;
    previewDirty = true;
    previewDebounce->start();
}

void QVBatchCompressDialog::launchPreview()
{
    if (running)
        return;
    if (previewBusy) {
        previewDirty = true;
        return;
    }

    const QString path = previewPath();
    if (path.isEmpty()) {
        previewBusy = false;
        previewDirty = false;
        preview->setPixmap(QPixmap());
        preview->setText(tr("Select an image to preview."));
        previewCaption->clear();
        previewSummary->clear();
        return;
    }

    previewBusy = true;
    previewDirty = false;
    preview->setPixmap(QPixmap());
    preview->setText(tr("Calculating…"));
    previewCaption->setText(QFileInfo(path).fileName());
    previewSummary->setText(tr("Calculating…"));

    const int quality = qualitySlider->value();
    const int mode = sizeMode->currentIndex();
    const int value = sizeValue->value();
    previewWatcher->setFuture(QtConcurrent::run([path, quality, mode, value]() {
        return encodeFile(path, quality, mode, value);
    }));
}

void QVBatchCompressDialog::showPreview(const QVBatchEncoded &encoded)
{
    previewCaption->setText(encoded.fileName);
    if (encoded.error == QLatin1String("animated")) {
        preview->setPixmap(QPixmap());
        preview->setText(tr("This animated image will be skipped."));
        previewSummary->clear();
        return;
    }
    if (encoded.error == QLatin1String("read") || encoded.image.isNull()) {
        preview->setPixmap(QPixmap());
        preview->setText(tr("Could not read this image."));
        previewSummary->clear();
        return;
    }
    if (!encoded.error.isEmpty() || encoded.encoded.isEmpty()) {
        preview->setPixmap(QPixmap());
        preview->setText(tr("Could not compress this image."));
        previewSummary->clear();
        return;
    }

    const qreal dpr = preview->devicePixelRatioF();
    QPixmap pix = QPixmap::fromImage(encoded.image);
    const QSize maxPx(qRound(preview->width() * dpr), qRound(preview->height() * dpr));
    if (pix.width() > maxPx.width() || pix.height() > maxPx.height())
        pix = pix.scaled(maxPx, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    pix.setDevicePixelRatio(dpr);
    preview->setText(QString());
    preview->setPixmap(pix);
    previewSummary->setText(tr("%1 x %2\n%3")
                                    .arg(encoded.image.width())
                                    .arg(encoded.image.height())
                                    .arg(compressionSummary(encoded.before, encoded.encoded.size())));
}

void QVBatchCompressDialog::startOrCancel()
{
    if (running) {
        if (cancelFlag)
            cancelFlag->store(true);
        startButton->setEnabled(false);
        return;
    }

    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;

    const bool overwrite = outputMode->currentIndex() == 1;
    if (currentModified && !currentPath.isEmpty() && paths.contains(currentPath)) {
        const QString detail = overwrite ? tr("Overwriting this file will discard those edits.")
                                         : tr("The new file will be made from the copy on disk.");
        const auto answer = QMessageBox::question(
                this, tr("Batch compress"),
                tr("This image has unsaved edits. Batch compress reads the file on disk and will "
                   "not include them.\n\n%1")
                        .arg(detail),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }
    if (overwrite) {
        const auto answer = QMessageBox::warning(
                this, tr("Batch compress"),
                tr("Overwrite %1 original files? This cannot be undone.").arg(paths.size()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }

    lastOverwrite = overwrite;
    running = true;
    closeAfterFinish = false;
    cancelFlag = std::make_shared<std::atomic_bool>(false);
    setInputsEnabled(false);
    startButton->setText(tr("Cancel"));
    startButton->setEnabled(true);
    if (QPushButton *closeButton = buttonBox->button(QDialogButtonBox::Close))
        closeButton->setEnabled(false);
    progressBar->setRange(0, paths.size());
    progressBar->setValue(0);
    progressBar->setVisible(true);
    statusLabel->setText(tr("Starting…"));

    delete worker;
    worker = nullptr;

    auto *thread = new QThread();
    workerThread = thread;
    auto progress = [this](int done, int total, const QString &name) {
        QMetaObject::invokeMethod(this, [this, done, total, name]() {
            reportProgress(done, total, name);
        }, Qt::QueuedConnection);
    };
    worker = new BatchCompressWorker(paths, qualitySlider->value(), sizeMode->currentIndex(),
                                     sizeValue->value(), overwrite, cancelFlag, thread,
                                     std::move(progress));
    // started() is emitted on the worker thread, before its event loop runs.
    connect(thread, &QThread::started, thread, [worker = this->worker]() { worker->run(); },
            Qt::DirectConnection);
    connect(thread, &QThread::finished, this, [this]() {
        const QVBatchJobResult result = worker ? worker->result : QVBatchJobResult();
        delete worker;
        worker = nullptr;
        workerThread = nullptr;
        showResult(result);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void QVBatchCompressDialog::reportProgress(int done, int total, const QString &fileName)
{
    if (!running)
        return;
    progressBar->setMaximum(qMax(1, total));
    progressBar->setValue(done);
    if (fileName.isEmpty())
        return;
    statusLabel->setText(tr("Compressing %1/%2: %3").arg(done + 1).arg(total).arg(fileName));
}

void QVBatchCompressDialog::setInputsEnabled(bool enabled)
{
    fileList->setEnabled(enabled);
    qualitySlider->setEnabled(enabled);
    sizeMode->setEnabled(enabled);
    sizeValue->setEnabled(enabled);
    outputMode->setEnabled(enabled);
    const auto buttons = findChildren<QPushButton *>();
    for (QPushButton *button : buttons) {
        if (button != startButton && buttonBox->button(QDialogButtonBox::Close) != button)
            button->setEnabled(enabled);
    }
}

void QVBatchCompressDialog::showResult(const QVBatchJobResult &result)
{
    running = false;
    if (result.cancelled)
        progressBar->setValue(result.succeeded + result.skipped + result.failed);
    else
        progressBar->setValue(progressBar->maximum());
    setInputsEnabled(true);
    startButton->setText(tr("Start"));
    updateSelectionLabel();
    if (QPushButton *closeButton = buttonBox->button(QDialogButtonBox::Close))
        closeButton->setEnabled(true);

    QStringList lines;
    if (result.cancelled) {
        lines.append(tr("Cancelled. %1 compressed, %2 skipped, %3 failed, %4 not processed.")
                             .arg(result.succeeded)
                             .arg(result.skipped)
                             .arg(result.failed)
                             .arg(result.remaining));
    } else {
        lines.append(tr("Finished. %1 compressed, %2 skipped, %3 failed.")
                             .arg(result.succeeded)
                             .arg(result.skipped)
                             .arg(result.failed));
    }
    if (result.skippedAnimated > 0)
        lines.append(tr("%1 animated images were skipped.").arg(result.skippedAnimated));
    if (result.succeeded > 0)
        lines.append(compressionSummary(result.before, result.after));
    if (!lastOverwrite && result.succeeded > 0) {
        lines.append(tr(
                "Copies were saved in a \"compressed\" folder next to each file."));
    }
    for (const QString &entry : result.errors) {
        const int split = entry.indexOf(QLatin1Char('\t'));
        const QString name = split < 0 ? entry : entry.left(split);
        const QString code = split < 0 ? QString() : entry.mid(split + 1);
        lines.append(tr("%1: %2").arg(name, describeError(code)));
    }
    const int hiddenFailures = result.failed - result.errors.size();
    if (hiddenFailures > 0)
        lines.append(tr("And %1 more failed.").arg(hiddenFailures));
    statusLabel->setText(lines.join(QLatin1Char('\n')));

    if (!result.overwritten.isEmpty())
        emit originalsOverwritten(result.overwritten);

    if (closeAfterFinish)
        accept();
}

QString QVBatchCompressDialog::describeError(const QString &code) const
{
    if (code == QLatin1String("read"))
        return tr("Could not read the image.");
    if (code == QLatin1String("encode"))
        return tr("Could not compress the image.");
    if (code == QLatin1String("mkdir"))
        return tr("Could not create the compressed folder.");
    if (code == QLatin1String("write"))
        return tr("Could not write the image.");
    return code;
}
