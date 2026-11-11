#include "qvbatchprintdialog.h"
#include "qvapplication.h"
#include "qvchecklistdelegate.h"
#include "qvprintpage.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {

// Reading image headers for the majority orientation is cheap but not free, so
// a very large folder falls back to the images seen so far.
const int kOrientationSampleLimit = 400;

// Decoded pages above this size stop helping the printer and start hurting.
const int kMaxDecodeEdge = 4000;
const int kMinDecodeEdge = 1024;

const int kMaxFailureLines = 8;

} // namespace

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
    resize(560, 640);

    auto *layout = new QVBoxLayout(this);

    layout->addWidget(new QLabel(tr("Files"), this));
    fileList = new QListWidget(this);
    fileList->setItemDelegate(new QVCheckListDelegate(fileList));
    fileList->setSelectionMode(QAbstractItemView::SingleSelection);
    fileList->setTextElideMode(Qt::ElideMiddle);
    fileList->setMinimumHeight(180);
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

    auto *hint = new QLabel(tr("One image per page, fitted to the paper and centered. Files that "
                               "cannot be read are skipped."),
                            this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *form = new QFormLayout();
    orientationMode = new QComboBox(this);
    orientationMode->addItem(tr("Automatic (majority of the selected images)"));
    orientationMode->addItem(tr("Portrait"));
    orientationMode->addItem(tr("Landscape"));
    form->addRow(tr("Orientation"), orientationMode);
    layout->addLayout(form);

    orientationNote = new QLabel(this);
    orientationNote->setWordWrap(true);
    layout->addWidget(orientationNote);

    progressBar = new QProgressBar(this);
    progressBar->setTextVisible(false);
    progressBar->setVisible(false);
    layout->addWidget(progressBar);

    statusLabel = new QLabel(this);
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
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
    connect(orientationMode, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { updateOrientationNote(); });
    connect(printButton, &QPushButton::clicked, this, &QVBatchPrintDialog::startPrint);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QVBatchPrintDialog::reject);

    {
        // Adding the initial list one item at a time would otherwise recompute
        // the orientation note for every single row.
        const QSignalBlocker blocker(fileList);
        for (const QString &path : folderFiles)
            addPath(path, true);
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

void QVBatchPrintDialog::closeEvent(QCloseEvent *event)
{
    if (running) {
        closeAfterFinish = true;
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void QVBatchPrintDialog::reject()
{
    if (running) {
        closeAfterFinish = true;
        return;
    }
    QDialog::reject();
}

void QVBatchPrintDialog::addPath(const QString &path, bool checked)
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
    for (int i = 0; i < fileList->count(); ++i)
        fileList->item(i)->setCheckState(checked ? Qt::Checked : Qt::Unchecked);

    updateSelectionLabel();
    updateOrientationNote();
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

void QVBatchPrintDialog::updateSelectionLabel()
{
    const int selected = selectedPaths().size();
    selectionLabel->setText(tr("%1 of %2 selected").arg(selected).arg(fileList->count()));
    printButton->setEnabled(selected > 0 && !running);
}

QSize QVBatchPrintDialog::imageSizeFor(const QString &path) const
{
    const auto cached = sizeCache.constFind(path);
    if (cached != sizeCache.constEnd())
        return cached.value();

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    sizeCache.insert(path, size);
    return size;
}

// The printer holds one page orientation for the whole job, so a mixed folder
// has to follow the majority and the rest are fitted with margins on the sides.
int QVBatchPrintDialog::landscapeCount(const QStringList &paths, int *known) const
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
    return landscape;
}

QPageLayout::Orientation QVBatchPrintDialog::autoOrientation() const
{
    int known = 0;
    const int landscape = landscapeCount(selectedPaths(), &known);
    if (known == 0)
        return QPageLayout::Portrait;
    return landscape * 2 > known ? QPageLayout::Landscape : QPageLayout::Portrait;
}

QPageLayout::Orientation QVBatchPrintDialog::chosenOrientation() const
{
    switch (orientationMode->currentIndex()) {
    case 1:
        return QPageLayout::Portrait;
    case 2:
        return QPageLayout::Landscape;
    default:
        return autoOrientation();
    }
}

void QVBatchPrintDialog::updateOrientationNote()
{
    const QStringList paths = selectedPaths();
    int known = 0;
    const int landscape = landscapeCount(paths, &known);

    QString text = tr("%1 selected: %2 landscape, %3 portrait.")
                           .arg(paths.size())
                           .arg(landscape)
                           .arg(qMax(0, known - landscape));
    if (orientationMode->currentIndex() == 0) {
        text += QLatin1Char(' ');
        text += tr("Automatic picks one orientation for the whole job, so the remaining images "
                   "are fitted with margins.");
    } else {
        text += QLatin1Char(' ');
        text += tr("Images of the other orientation are fitted with margins.");
    }
    orientationNote->setText(text);
}

QImage QVBatchPrintDialog::decodeForPrint(const QString &path, const QSize &box) const
{
    if (!currentImage.isNull() && path == currentPath)
        return currentImage;

    QImageReader reader(path);
    reader.setAutoTransform(true);

    const QSize source = reader.size();
    if (source.isValid() && !source.isEmpty()) {
        QSize scaled = source;
        scaled.scale(box, Qt::KeepAspectRatio);
        if (scaled.isValid() && !scaled.isEmpty() && scaled != source)
            reader.setScaledSize(scaled);
    }

    return reader.read();
}

void QVBatchPrintDialog::setInputsEnabled(bool enabled)
{
    fileList->setEnabled(enabled);
    orientationMode->setEnabled(enabled);
    orientationNote->setEnabled(enabled);
    printButton->setEnabled(enabled && !selectedPaths().isEmpty());

    if (QPushButton *closeButton = buttonBox->button(QDialogButtonBox::Close))
        closeButton->setEnabled(enabled);
}

void QVBatchPrintDialog::startPrint()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty()) {
        QMessageBox::information(this, tr("Batch print"), tr("Select at least one image."));
        return;
    }

    QPrinter printer(QPrinter::HighResolution);
    printer.setPageOrientation(chosenOrientation());

    QPrintDialog dialog(&printer, this);
    dialog.setWindowTitle(tr("Batch print"));
    if (dialog.exec() != QDialog::Accepted)
        return;

    QPainter painter;
    if (!painter.begin(&printer)) {
        QMessageBox::warning(this, tr("Print"), tr("Could not start printing."));
        return;
    }

    // Asked for after begin() to match the single image print, and because the
    // dialog may have changed the paper size.
    const QRectF pageRect = printer.pageLayout().paintRectPixels(printer.resolution());
    if (pageRect.isEmpty()) {
        painter.end();
        QMessageBox::warning(this, tr("Print"), tr("Could not start printing."));
        return;
    }

    // Decode near the printable resolution: a 6000x4000 photo is 92 MB once
    // decoded and the page cannot show more detail than this. The box is square
    // because an EXIF rotation can swap the sides of the image.
    const int pageLongest = qMax(qRound(pageRect.width()), qRound(pageRect.height()));
    const QSize box(qBound(kMinDecodeEdge, pageLongest, kMaxDecodeEdge),
                    qBound(kMinDecodeEdge, pageLongest, kMaxDecodeEdge));

    running = true;
    setInputsEnabled(false);
    statusLabel->setVisible(true);
    progressBar->setRange(0, paths.size());
    progressBar->setValue(0);
    progressBar->setVisible(true);

    int printed = 0;
    QStringList failed;
    bool firstPage = true;

    for (int i = 0; i < paths.size(); ++i) {
        const QString path = paths.at(i);
        statusLabel->setText(tr("Printing %1 of %2: %3")
                                     .arg(i + 1)
                                     .arg(paths.size())
                                     .arg(QFileInfo(path).fileName()));
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

        const QImage image = decodeForPrint(path, box);
        if (image.isNull()) {
            failed.append(QFileInfo(path).fileName());
            progressBar->setValue(i + 1);
            continue;
        }

        // Only move to a new page once there is something to put on it.
        if (!firstPage && !printer.newPage()) {
            failed.append(QFileInfo(path).fileName());
            break;
        }
        firstPage = false;

        drawImageOnPage(painter, pageRect, image);
        ++printed;
        progressBar->setValue(i + 1);
    }

    painter.end();

    running = false;
    progressBar->setVisible(false);
    setInputsEnabled(true);

    if (printed == 0) {
        statusLabel->setText(tr("Nothing was printed."));
    } else {
        statusLabel->setText(
                tr("Sent %1 of %2 images to the printer.").arg(printed).arg(paths.size()));
    }

    if (!failed.isEmpty()) {
        const int shown = qMin(failed.size(), kMaxFailureLines);
        QStringList lines = failed.mid(0, shown);
        if (failed.size() > shown)
            lines.append(tr("…and %1 more").arg(failed.size() - shown));

        QMessageBox::warning(
                this, tr("Batch print"),
                tr("These files could not be printed:\n%1").arg(lines.join(QLatin1Char('\n'))));
    }

    if (closeAfterFinish) {
        closeAfterFinish = false;
        close();
    }
}
