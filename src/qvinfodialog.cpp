#include "qvinfodialog.h"
#include "ui_qvinfodialog.h"
#include <QDateTime>
#include <QMimeDatabase>
#include <QTimer>

static int getGcd(int a, int b)
{
    return (b == 0) ? a : getGcd(b, a % b);
}

QVInfoDialog::QVInfoDialog(QWidget *parent) : QDialog(parent), ui(new Ui::QVInfoDialog)
{
    ui->setupUi(this);
    setWindowFlags(windowFlags() & (~Qt::WindowContextHelpButtonHint | Qt::CustomizeWindowHint));

    width = 0;
    height = 0;
    frameCount = 0;
}

QVInfoDialog::~QVInfoDialog()
{
    delete ui;
}

void QVInfoDialog::setInfo(const QFileInfo &value, const int &value2, const int &value3,
                           const int &value4)
{
    selectedFileInfo = value;
    width = value2;
    height = value3;
    frameCount = value4;

    // If the dialog is visible, it means we've just navigated to a new image. Instead of running
    // updateInfo immediately, add it to the event queue. This is a workaround for a
    // (Windows-specific?) delay when calling adjustSize on the window if the font contains certain
    // characters (e.g. Chinese) the first time that happens for a given font. At least on Windows,
    // by making the work happen later in the event loop, it allows the main window to repaint
    // first, giving the appearance of better responsiveness. If the dialog is not visible, however,
    // it means we're preparing to display for an image already opened. In this case there is no
    // urgency to repaint the main window, and we need to process the updates here synchronously to
    // avoid the caller showing the dialog before it's ready (i.e. to avoid showing outdated info or
    // placeholder text).
    if (isVisible())
        QTimer::singleShot(0, this, &QVInfoDialog::updateInfo);
    else
        updateInfo();
}

QVFileInfoFields QVInfoDialog::buildFields(const QFileInfo &fileInfo, int width, int height,
                                            int frameCount)
{
    QLocale locale = QLocale::system();
    QMimeDatabase mimedb;
    QMimeType mime =
            mimedb.mimeTypeForFile(fileInfo.absoluteFilePath(), QMimeDatabase::MatchContent);
    // this is just math to figure the megapixels and then round it to the tenths place
    const double megapixels =
            static_cast<double>(
                    qRound(((static_cast<double>((width * height)))) / 1000000 * 10 + 0.5))
            / 10;

    QVFileInfoFields fields;
    fields.name = fileInfo.fileName();
    fields.type = mime.name();
    fields.location = fileInfo.path();
    fields.size = tr("%1 (%2 bytes)")
                          .arg(formatBytes(fileInfo.size()), locale.toString(fileInfo.size()));
    fields.modified = fileInfo.lastModified().toString(locale.dateTimeFormat());
    fields.dimensions = tr("%1 x %2 (%3 MP)")
                                .arg(QString::number(width), QString::number(height),
                                     QString::number(megapixels));
    const int gcd = getGcd(width, height);
    if (gcd != 0)
        fields.ratio = QString::number(width / gcd) + ":" + QString::number(height / gcd);
    if (frameCount != 0)
        fields.frames = QString::number(frameCount);
    return fields;
}

void QVInfoDialog::updateInfo()
{
    const QVFileInfoFields fields = buildFields(selectedFileInfo, width, height, frameCount);

    ui->nameLabel->setText(fields.name);
    ui->typeLabel->setText(fields.type);
    ui->locationLabel->setText(fields.location);
    ui->sizeLabel->setText(fields.size);
    ui->modifiedLabel->setText(fields.modified);
    ui->dimensionsLabel->setText(fields.dimensions);
    ui->ratioLabel->setText(fields.ratio);
    if (!fields.frames.isEmpty()) {
        ui->framesLabel2->show();
        ui->framesLabel->show();
        ui->framesLabel->setText(fields.frames);
    } else {
        ui->framesLabel2->hide();
        ui->framesLabel->hide();
    }
    window()->adjustSize();
}
