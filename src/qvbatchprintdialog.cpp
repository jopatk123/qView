#include "qvbatchprintdialog.h"
#include "qvapplication.h"
#include "qvchecklistdelegate.h"
#include "qvprintpage.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImageIOHandler>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPagedPaintDevice>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QPrintDialog>
#include <QPrinter>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// Reading image headers for the majority orientation is cheap but not free, so
// a very large folder falls back to a sample of the first files. The note below
// the list says so instead of pretending the sample covers everything.
const int kOrientationSampleLimit = 400;

// Decoded pages above this size stop helping the printer and start hurting.
const int kMaxDecodeEdge = 4000;

const int kMaxFailureLines = 8;

// One image per page means a large selection costs paper and toner, so a big
// job asks before the print dialog opens.
const int kConfirmAbovePages = 20;

const int kPdfResolution = 300;

// Small enough to decode on every selection change without a stall.
const int kPreviewEdge = 400;

} // namespace

// A miniature of the printed page. The system print panel only appears once the
// job starts, so this is the one place to see how the chosen paper direction
// treats the current image before anything is sent.
class QVPrintPagePreview : public QWidget
{
public:
    explicit QVPrintPagePreview(QWidget *parent = nullptr) : QWidget(parent)
    {
        // Fixed so the page shape does not jump around while the window resizes.
        setFixedSize(224, 268);
    }

    void setImage(const QImage &image)
    {
        preview = image;
        update();
    }

    void setOrientation(QPageLayout::Orientation value)
    {
        if (orientation == value)
            return;
        orientation = value;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().color(QPalette::Window));

        const QRectF area = QRectF(rect()).adjusted(6, 6, -6, -6);
        if (area.width() <= 0 || area.height() <= 0)
            return;

        // A4 stands in for the paper until the print panel chooses the real one;
        // only the shape matters here.
        QSizeF page(210, 297);
        if (orientation == QPageLayout::Landscape)
            page.transpose();
        page.scale(area.size(), Qt::KeepAspectRatio);

        const QRectF pageRect(area.center().x() - page.width() / 2.0,
                              area.center().y() - page.height() / 2.0, page.width(),
                              page.height());
        painter.fillRect(pageRect, Qt::white);
        painter.setPen(QColor(165, 165, 165));
        painter.drawRect(pageRect.adjusted(0.5, 0.5, -0.5, -0.5));

        if (preview.isNull())
            return;

        QSizeF target(preview.size());
        target.scale(pageRect.size(), Qt::KeepAspectRatio);
        const QRectF targetRect(pageRect.x() + (pageRect.width() - target.width()) / 2.0,
                                pageRect.y() + (pageRect.height() - target.height()) / 2.0,
                                target.width(), target.height());
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(targetRect, preview);
    }

private:
    QImage preview;
    QPageLayout::Orientation orientation = QPageLayout::Portrait;
};

QVBatchPrintDialog::QVBatchPrintDialog(const QStringList &folderFiles,
                                       const QString &currentFilePath, const QImage &liveImage,
                                       QWidget *parent)
    : QDialog(parent),
      currentPath(QFileInfo(currentFilePath).absoluteFilePath()),
      currentImage(liveImage)
{
    setWindowTitle(tr("Batch print"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setWindowModality(Qt::WindowModal);
    resize(780, 660);

    auto *layout = new QVBoxLayout(this);
    auto *columns = new QHBoxLayout();

    auto *listColumn = new QVBoxLayout();
    listColumn->addWidget(new QLabel(tr("Files"), this));
    fileList = new QListWidget(this);
    auto *checkDelegate = new QVCheckListDelegate(fileList);
    // Nothing else in this list reacts to a click, so the whole row toggles.
    checkDelegate->setToggleOnRowClick(true);
    fileList->setItemDelegate(checkDelegate);
    fileList->setSelectionMode(QAbstractItemView::SingleSelection);
    fileList->setTextElideMode(Qt::ElideMiddle);
    fileList->setMinimumHeight(180);
    listColumn->addWidget(fileList, 1);

    auto *fileButtons = new QHBoxLayout();
    addButton = new QPushButton(tr("Add files…"), this);
    allButton = new QPushButton(tr("All"), this);
    noneButton = new QPushButton(tr("None"), this);
    selectionLabel = new QLabel(this);
    fileButtons->addWidget(addButton);
    fileButtons->addWidget(allButton);
    fileButtons->addWidget(noneButton);
    fileButtons->addStretch(1);
    fileButtons->addWidget(selectionLabel);
    listColumn->addLayout(fileButtons);
    columns->addLayout(listColumn, 1);

    auto *previewColumn = new QVBoxLayout();
    previewColumn->addWidget(new QLabel(tr("Preview"), this));
    pagePreview = new QVPrintPagePreview(this);
    previewColumn->addWidget(pagePreview, 0, Qt::AlignHCenter);
    previewCaption = new QLabel(this);
    previewCaption->setAlignment(Qt::AlignCenter);
    previewCaption->setWordWrap(true);
    previewColumn->addWidget(previewCaption);
    previewColumn->addStretch(1);
    columns->addLayout(previewColumn);

    layout->addLayout(columns, 1);

    auto *hint = new QLabel(tr("One image per page, fitted to the paper and centered. Files that "
                               "cannot be read are skipped."),
                            this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *form = new QFormLayout();
    orientationMode = new QComboBox(this);
    orientationMode->addItem(tr("Adaptive (paper direction follows each image)"));
    orientationMode->addItem(tr("Portrait"));
    orientationMode->addItem(tr("Landscape"));
    {
        QSettings settings;
        const int saved = settings.value(QStringLiteral("batchPrint/orientationMode"), 0).toInt();
        if (saved > 0 && saved < orientationMode->count())
            orientationMode->setCurrentIndex(saved);
    }
    form->addRow(tr("Orientation"), orientationMode);
    layout->addLayout(form);

    orientationNote = new QLabel(this);
    orientationNote->setWordWrap(true);
    layout->addWidget(orientationNote);

    progressBar = new QProgressBar(this);
    progressBar->setTextVisible(true);
    progressBar->setFormat(QStringLiteral("%v / %m"));
    progressBar->setVisible(false);
    layout->addWidget(progressBar);

    statusLabel = new QLabel(this);
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    pdfButton = buttonBox->addButton(tr("Save as PDF…"), QDialogButtonBox::ActionRole);
    printButton = buttonBox->addButton(tr("Print…"), QDialogButtonBox::ActionRole);
    printButton->setDefault(true);
    layout->addWidget(buttonBox);

    connect(addButton, &QPushButton::clicked, this, &QVBatchPrintDialog::addFiles);
    connect(allButton, &QPushButton::clicked, this, [this]() { setAllChecked(true); });
    connect(noneButton, &QPushButton::clicked, this, [this]() { setAllChecked(false); });
    connect(fileList, &QListWidget::itemChanged, this, [this](QListWidgetItem *) {
        updateSelectionLabel();
        updateOrientationNote();
    });
    connect(fileList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *, QListWidgetItem *) { updatePreview(); });
    connect(orientationMode, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                storeOrientationChoice(index);
                updateOrientationNote();
            });
    connect(printButton, &QPushButton::clicked, this, &QVBatchPrintDialog::handlePrimaryButton);
    connect(pdfButton, &QPushButton::clicked, this, &QVBatchPrintDialog::startPdfExport);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QVBatchPrintDialog::reject);

    {
        // Adding the initial list one item at a time would otherwise recompute
        // the orientation note for every single row. Only the open image starts
        // checked: a folder of several hundred photos should never be one Enter
        // key away from the printer.
        const QSignalBlocker blocker(fileList);
        for (const QString &path : folderFiles)
            addPath(path, QFileInfo(path).absoluteFilePath() == currentPath);
    }
    updateSelectionLabel();
    updateOrientationNote();

    if (!currentPath.isEmpty()) {
        for (int i = 0; i < fileList->count(); ++i) {
            if (fileList->item(i)->data(Qt::UserRole).toString() == currentPath) {
                fileList->setCurrentRow(i);
                fileList->scrollToItem(fileList->item(i));
                break;
            }
        }
    }
}

QVBatchPrintDialog::~QVBatchPrintDialog()
{
    // A job only outlives the dialog when the window is torn down mid-run and
    // the pending page callback never fires.
    if (jobPainter) {
        if (jobPainter->isActive())
            jobPainter->end();
        delete jobPainter;
    }
    delete jobDevice;
}

void QVBatchPrintDialog::closeEvent(QCloseEvent *event)
{
    if (running) {
        requestCancel();
        closeAfterFinish = true;
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void QVBatchPrintDialog::reject()
{
    if (running) {
        requestCancel();
        closeAfterFinish = true;
        return;
    }
    QDialog::reject();
}

void QVBatchPrintDialog::addPath(const QString &path, bool checked)
{
    const QFileInfo info(path);
    const QString absolute = info.absoluteFilePath();
    if (absolute.isEmpty() || !info.isFile())
        return;
    // A folder with thousands of images made the per-add scan quadratic.
    if (knownPaths.contains(absolute))
        return;
    knownPaths.insert(absolute);

    auto *item = new QListWidgetItem(info.fileName(), fileList);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    item->setData(Qt::UserRole, absolute);
    item->setToolTip(absolute);
}

void QVBatchPrintDialog::addFiles()
{
    QSettings settings;
    settings.beginGroup("recents");
    QString directory = settings.value("lastFileDialogDir", QDir::homePath()).toString();
    if (!currentPath.isEmpty())
        directory = QFileInfo(currentPath).absolutePath();

    const QStringList picked =
            QFileDialog::getOpenFileNames(this, tr("Add files"), directory,
                                          qvApp->getNameFilterList().join(QStringLiteral(";;")));
    if (picked.isEmpty())
        return;

    settings.setValue("lastFileDialogDir", QFileInfo(picked.constFirst()).absolutePath());

    const QSignalBlocker blocker(fileList);
    for (const QString &path : picked)
        addPath(path, true);

    updateSelectionLabel();
    updateOrientationNote();
}

void QVBatchPrintDialog::setAllChecked(bool checked)
{
    const QSignalBlocker blocker(fileList);
    const auto state = checked ? Qt::Checked : Qt::Unchecked;
    for (int i = 0; i < fileList->count(); ++i)
        fileList->item(i)->setCheckState(state);

    updateSelectionLabel();
    updateOrientationNote();
}

int QVBatchPrintDialog::selectedCount() const
{
    int count = 0;
    for (int i = 0; i < fileList->count(); ++i) {
        if (fileList->item(i)->checkState() == Qt::Checked)
            ++count;
    }
    return count;
}

QStringList QVBatchPrintDialog::selectedPaths() const
{
    QStringList paths;
    for (int i = 0; i < fileList->count(); ++i) {
        const QListWidgetItem *item = fileList->item(i);
        if (item->checkState() == Qt::Checked)
            paths.append(item->data(Qt::UserRole).toString());
    }
    return paths;
}

QString QVBatchPrintDialog::previewPath() const
{
    if (const QListWidgetItem *item = fileList->currentItem())
        return item->data(Qt::UserRole).toString();
    const QStringList selected = selectedPaths();
    return selected.isEmpty() ? QString() : selected.constFirst();
}

void QVBatchPrintDialog::updatePreview()
{
    const QString path = previewPath();

    // Decoding is only worth repeating when the picture changed; switching the
    // paper direction just repaints the same preview.
    if (path != previewPathCache) {
        previewPathCache = path;
        previewImage = QImage();
        if (!path.isEmpty()) {
            if (path == currentPath && !currentImage.isNull()) {
                // Show the edited image, since that is what will be printed.
                previewImage = currentImage;
                if (previewImage.width() > kPreviewEdge || previewImage.height() > kPreviewEdge) {
                    previewImage = previewImage.scaled(kPreviewEdge, kPreviewEdge,
                                                       Qt::KeepAspectRatio,
                                                       Qt::SmoothTransformation);
                }
            } else {
                QImageReader reader(path);
                reader.setAutoTransform(true);
                const QSize source = reader.size();
                if (source.isValid() && !source.isEmpty()) {
                    QSize target = source;
                    target.scale(kPreviewEdge, kPreviewEdge, Qt::KeepAspectRatio);
                    if (target.width() < source.width())
                        reader.setScaledSize(target);
                }
                previewImage = reader.read();
            }
        }
        pagePreview->setImage(previewImage);
    }

    const QPageLayout::Orientation orientation =
            orientationMode->currentIndex() == 0 ? orientationFor(path) : chosenOrientation();
    pagePreview->setOrientation(orientation);

    if (path.isEmpty()) {
        previewCaption->setText(tr("Select an image to see the page."));
        return;
    }

    const QSize display = imageSizeFor(path);
    const QString paper =
            orientation == QPageLayout::Landscape ? tr("Landscape") : tr("Portrait");
    previewCaption->setText(tr("Paper: %1\nImage: %2 × %3")
                                    .arg(paper, QString::number(display.width()),
                                         QString::number(display.height())));
}

void QVBatchPrintDialog::updateSelectionLabel()
{
    const int selected = selectedCount();
    selectionLabel->setText(tr("%1 of %2 selected").arg(selected).arg(fileList->count()));
    if (!running) {
        printButton->setEnabled(selected > 0);
        pdfButton->setEnabled(selected > 0);
    }
}

QSize QVBatchPrintDialog::imageSizeFor(const QString &path) const
{
    const auto cached = sizeCache.constFind(path);
    if (cached != sizeCache.constEnd())
        return cached.value();

    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize size = reader.size();
    // read() applies the EXIF rotation but size() reports the stored layout, so
    // without this a photo shot in portrait and flagged "rotate 90" counts as
    // landscape and the paper direction comes out wrong for the whole job.
    if (reader.transformation() & QImageIOHandler::TransformationRotate90)
        size.transpose();
    sizeCache.insert(path, size);
    return size;
}

// The printer holds one page orientation for the whole job, so a mixed folder
// has to follow the majority and the rest are fitted with margins on the sides.
int QVBatchPrintDialog::landscapeCount(const QStringList &paths, int *known, int *sampled) const
{
    int landscape = 0;
    int measured = 0;
    const int limit = qMin(paths.size(), kOrientationSampleLimit);
    for (int i = 0; i < limit; ++i) {
        const QSize size = imageSizeFor(paths.at(i));
        if (!size.isValid() || size.isEmpty())
            continue;
        ++measured;
        if (size.width() > size.height())
            ++landscape;
    }

    if (known)
        *known = measured;
    if (sampled)
        *sampled = limit;
    return landscape;
}

QPageLayout::Orientation QVBatchPrintDialog::orientationFor(const QString &path) const
{
    const QSize size = imageSizeFor(path);
    if (!size.isValid() || size.isEmpty())
        return QPageLayout::Portrait;
    return size.width() > size.height() ? QPageLayout::Landscape : QPageLayout::Portrait;
}

QPageLayout::Orientation QVBatchPrintDialog::chosenOrientation() const
{
    switch (orientationMode->currentIndex()) {
    case 1:
        return QPageLayout::Portrait;
    case 2:
        return QPageLayout::Landscape;
    default: {
        // Adaptive: the job starts on whatever the first page needs.
        const QStringList paths = selectedPaths();
        return orientationFor(paths.isEmpty() ? previewPath() : paths.constFirst());
    }
    }
}

void QVBatchPrintDialog::storeOrientationChoice(int index)
{
    QSettings settings;
    settings.setValue(QStringLiteral("batchPrint/orientationMode"), index);
}

void QVBatchPrintDialog::updateOrientationNote()
{
    // The preview follows the same decision, so it is refreshed first: whatever
    // the note says about automatic is also what the paper shape shows.
    updatePreview();

    const QStringList paths = selectedPaths();
    if (paths.isEmpty()) {
        orientationNote->setText(tr("Select at least one image."));
        return;
    }

    int known = 0;
    int sampled = 0;
    const int landscape = landscapeCount(paths, &known, &sampled);
    const int portrait = known - landscape;
    const int unreadable = sampled - known;

    QString text;
    if (sampled < paths.size()) {
        // Only the first files were inspected, so these counts describe the
        // sample and must not be presented as the whole selection.
        text = tr("%1 selected. From the first %2 files: %3 landscape, %4 portrait.")
                       .arg(paths.size())
                       .arg(sampled)
                       .arg(landscape)
                       .arg(portrait);
    } else {
        text = tr("%1 selected: %2 landscape, %3 portrait.")
                       .arg(paths.size())
                       .arg(landscape)
                       .arg(portrait);
    }
    if (unreadable > 0)
        text += QLatin1Char(' ') + tr("%1 could not be read.").arg(unreadable);

    text += QLatin1Char(' ');
    if (orientationMode->currentIndex() == 0) {
        text += tr("Each image gets the paper direction that fits it, so nothing is fitted with "
                   "margins.");
    } else {
        text += tr("Images of the other orientation are fitted with margins.");
    }
    orientationNote->setText(text);
}

QImage QVBatchPrintDialog::decodeForPrint(const QString &path, int edge) const
{
    if (!currentImage.isNull() && path == currentPath)
        return currentImage;

    QImageReader reader(path);
    reader.setAutoTransform(true);

    const QSize source = reader.size();
    if (source.isValid() && !source.isEmpty()) {
        // Only ever scale down. Requesting a larger size makes the reader
        // stretch a small image, which costs memory and adds no detail.
        const qreal fit = qMin(qreal(edge) / source.width(), qreal(edge) / source.height());
        if (fit < 1.0) {
            const QSize target(qMax(1, qRound(source.width() * fit)),
                               qMax(1, qRound(source.height() * fit)));
            if (target.isValid() && !target.isEmpty())
                reader.setScaledSize(target);
        }
    }

    return reader.read();
}

void QVBatchPrintDialog::handlePrimaryButton()
{
    if (running)
        requestCancel();
    else
        startPrint();
}

void QVBatchPrintDialog::startPrint()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty()) {
        QMessageBox::information(this, tr("Batch print"), tr("Select at least one image."));
        return;
    }

    if (paths.size() > kConfirmAbovePages) {
        const auto answer =
                QMessageBox::question(this, tr("Batch print"),
                                      tr("%1 pages will be printed, one image per page. Continue?")
                                              .arg(paths.size()),
                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }

    // The job outlives this function, so the device is heap allocated and owned
    // by the dialog until finishJob().
    auto *printer = new QPrinter(QPrinter::HighResolution);
    printer->setPageOrientation(chosenOrientation());
    printer->setDocName(tr("Batch print"));

    QPrintDialog dialog(printer, this);
    dialog.setWindowTitle(tr("Batch print"));
    if (dialog.exec() != QDialog::Accepted) {
        delete printer;
        return;
    }

    auto *painter = new QPainter();
    if (!painter->begin(printer)) {
        delete painter;
        delete printer;
        QMessageBox::warning(this, tr("Print"), tr("Could not start printing."));
        return;
    }

    jobToPdf = false;
    jobDestination.clear();
    // Asked for after begin() to match the single image print, and because the
    // dialog may have changed the paper size or the orientation.
    startJob(painter, printer, printer->pageLayout().paintRectPixels(printer->resolution()),
             printer->resolution());
}

void QVBatchPrintDialog::startPdfExport()
{
    const QStringList selected = selectedPaths();
    if (selected.isEmpty()) {
        QMessageBox::information(this, tr("Save as PDF"), tr("Select at least one image."));
        return;
    }

    QString directory = QDir::homePath();
    QString baseName = tr("Batch print");
    if (!currentPath.isEmpty()) {
        const QFileInfo info(currentPath);
        directory = info.absolutePath();
        const QString folderName = info.dir().dirName();
        baseName = folderName.isEmpty() ? info.completeBaseName() : folderName;
    }

    QString filePath = QFileDialog::getSaveFileName(
            this, tr("Save as PDF"),
            directory + QLatin1Char('/') + baseName + QStringLiteral(".pdf"),
            tr("PDF Files (*.pdf)"));
    if (filePath.isEmpty())
        return;
    bool appendedSuffix = false;
    if (!filePath.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
        filePath += QStringLiteral(".pdf");
        appendedSuffix = true;
    }
    // The system save panel only warns when the name it was given already ends
    // in .pdf, so a typed name that gains the suffix needs its own check.
    if (appendedSuffix && QFileInfo::exists(filePath)) {
        const auto answer = QMessageBox::question(
                this, tr("Save as PDF"),
                tr("%1 already exists. Do you want to replace it?")
                        .arg(QFileInfo(filePath).fileName()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }

    auto *writer = new QPdfWriter(filePath);
    writer->setPageSize(QPageSize(QPageSize::A4));
    writer->setPageOrientation(chosenOrientation());
    writer->setResolution(kPdfResolution);
    writer->setTitle(baseName);
    writer->setCreator(QStringLiteral("qView"));

    auto *painter = new QPainter();
    if (!painter->begin(writer)) {
        delete painter;
        delete writer;
        QMessageBox::warning(this, tr("Save as PDF"), tr("Could not create the PDF file."));
        return;
    }

    jobToPdf = true;
    jobDestination = QFileInfo(filePath).fileName();
    startJob(painter, writer, writer->pageLayout().paintRectPixels(writer->resolution()),
             writer->resolution());
}

void QVBatchPrintDialog::startJob(QPainter *painter, QPagedPaintDevice *device,
                                  const QRectF &pageRect, int resolution)
{
    if (pageRect.isEmpty()) {
        if (painter->isActive())
            painter->end();
        delete painter;
        delete device;
        QMessageBox::warning(this, jobToPdf ? tr("Save as PDF") : tr("Print"),
                             tr("Could not start printing."));
        return;
    }

    jobPaths = selectedPaths();
    jobPainter = painter;
    jobDevice = device;
    jobPageRect = pageRect;
    jobResolution = resolution;
    jobIndex = 0;
    jobPrinted = 0;
    jobRemaining = 0;
    jobFailed.clear();
    jobFirstPage = true;
    cancelRequested = false;

    // Decode near the printable resolution: a 6000x4000 photo is 92 MB once
    // decoded and the page cannot show more detail than this. The cap is square
    // because an EXIF rotation can swap the sides of the image, and
    // decodeForPrint() never scales up, so this only ever removes pixels. The
    // longest side of the paper is the same in both directions, so one cap
    // covers every page of an adaptive job.
    const int pageLongest = qMax(qRound(pageRect.width()), qRound(pageRect.height()));
    jobEdge = qMax(1, qMin(pageLongest, kMaxDecodeEdge));

    running = true;
    closeAfterFinish = false;
    setInputsEnabled(false);
    printButton->setText(tr("Cancel"));
    printButton->setEnabled(true);
    progressBar->setRange(0, jobPaths.size());
    progressBar->setValue(0);
    progressBar->setVisible(true);
    statusLabel->setText(tr("Preparing…"));

    // Every page comes back through the event loop, so the window keeps
    // repainting and the Cancel button keeps working.
    QTimer::singleShot(0, this, &QVBatchPrintDialog::printNextPage);
}

void QVBatchPrintDialog::applyOrientationFor(const QString &path)
{
    if (!jobDevice || orientationMode->currentIndex() != 0)
        return;

    const QPageLayout::Orientation wanted = orientationFor(path);
    if (jobDevice->pageLayout().orientation() != wanted)
        jobDevice->setPageOrientation(wanted);
}

QRectF QVBatchPrintDialog::currentPageRect() const
{
    // Read back after every newPage(): the orientation may just have changed, and
    // the print panel may have picked a different paper size entirely.
    return jobDevice ? jobDevice->pageLayout().paintRectPixels(jobResolution) : QRectF();
}

void QVBatchPrintDialog::printNextPage()
{
    if (!running)
        return;

    if (cancelRequested) {
        jobRemaining = jobPaths.size() - jobIndex;
        finishJob(false);
        return;
    }

    if (jobIndex >= jobPaths.size()) {
        finishJob(false);
        return;
    }

    const QString path = jobPaths.at(jobIndex);
    const QString name = QFileInfo(path).fileName();
    statusLabel->setText(jobToPdf
                                 ? tr("Saving %1 of %2: %3")
                                           .arg(jobIndex + 1)
                                           .arg(jobPaths.size())
                                           .arg(name)
                                 : tr("Printing %1 of %2: %3")
                                           .arg(jobIndex + 1)
                                           .arg(jobPaths.size())
                                           .arg(name));

    const QImage image = decodeForPrint(path, jobEdge);
    ++jobIndex;

    if (image.isNull()) {
        jobFailed.append(name);
    } else {
        if (!jobFirstPage) {
            // A new page is the only place where the paper direction can change,
            // which is what makes one job work for a mixed folder.
            applyOrientationFor(path);
            if (!jobDevice->newPage()) {
                // The device stopped taking pages. Everything before this point
                // is already spooled, so report what is left instead of blaming
                // the file.
                jobRemaining = jobPaths.size() - jobIndex;
                finishJob(true);
                return;
            }
            jobPageRect = currentPageRect();
        }
        jobFirstPage = false;
        drawImageOnPage(*jobPainter, jobPageRect, image);
        ++jobPrinted;
    }

    progressBar->setValue(jobIndex);
    QTimer::singleShot(0, this, &QVBatchPrintDialog::printNextPage);
}

void QVBatchPrintDialog::requestCancel()
{
    if (!running || cancelRequested)
        return;
    cancelRequested = true;
    printButton->setText(tr("Cancelling…"));
    printButton->setEnabled(false);
    statusLabel->setText(tr("Cancelling…"));
}

void QVBatchPrintDialog::finishJob(bool deviceFailed)
{
    running = false;

    if (jobPainter) {
        if (jobPainter->isActive())
            jobPainter->end();
        delete jobPainter;
        jobPainter = nullptr;
    }
    delete jobDevice;
    jobDevice = nullptr;

    progressBar->setVisible(false);
    setInputsEnabled(true);
    printButton->setText(tr("Print…"));
    updateSelectionLabel();

    const int total = jobPaths.size();
    QString summary;
    if (deviceFailed) {
        summary = tr("Printing stopped after %1 of %2 images: the printer would not take another "
                     "page. %3 images were not sent.")
                          .arg(jobPrinted)
                          .arg(total)
                          .arg(jobRemaining);
    } else if (cancelRequested) {
        summary = tr("Cancelled after %1 of %2 images.").arg(jobPrinted).arg(total);
    } else if (jobToPdf) {
        summary =
                tr("Wrote %1 of %2 images to %3.").arg(jobPrinted).arg(total).arg(jobDestination);
    } else if (jobPrinted == 0) {
        summary = tr("Nothing was printed.");
    } else {
        summary = tr("Sent %1 of %2 images to the printer.").arg(jobPrinted).arg(total);
    }

    QString failures;
    if (!jobFailed.isEmpty()) {
        const int shown = qMin(jobFailed.size(), kMaxFailureLines);
        QStringList lines = jobFailed.mid(0, shown);
        if (jobFailed.size() > shown)
            lines.append(tr("…and %1 more").arg(jobFailed.size() - shown));
        failures = tr("These files could not be printed:\n%1").arg(lines.join(QLatin1Char('\n')));
    }

    statusLabel->setText(failures.isEmpty() ? summary : summary + QLatin1Char('\n') + failures);

    // The dialog is on its way out when the user closed it during the job, so a
    // modal warning on top of that would only block the close.
    if (!failures.isEmpty() && !closeAfterFinish)
        QMessageBox::warning(this, jobToPdf ? tr("Save as PDF") : tr("Batch print"), failures);

    if (closeAfterFinish) {
        closeAfterFinish = false;
        close();
    }
}

void QVBatchPrintDialog::setInputsEnabled(bool enabled)
{
    fileList->setEnabled(enabled);
    orientationMode->setEnabled(enabled);
    orientationNote->setEnabled(enabled);
    addButton->setEnabled(enabled);
    allButton->setEnabled(enabled);
    noneButton->setEnabled(enabled);
    pdfButton->setEnabled(enabled && selectedCount() > 0);
    printButton->setEnabled(enabled && selectedCount() > 0);

    if (QPushButton *closeButton = buttonBox->button(QDialogButtonBox::Close))
        closeButton->setEnabled(enabled);
}
