#include "qvfileinfopanel.h"
#include "qvapplication.h"

#include <QFormLayout>
#include <QLabel>
#include <QPainter>
#include <QScrollArea>
#include <QVBoxLayout>

static QString softWrap(const QString &text, const QFontMetrics &metrics, int width)
{
    if (text.isEmpty() || width <= 32)
        return text;

    QString wrapped;
    QString line;
    for (int i = 0; i < text.size(); ++i) {
        line.append(text.at(i));
        if (metrics.boundingRect(line).width() <= width)
            continue;
        if (line.size() == 1)
            continue;
        line.chop(1);
        wrapped += line;
        wrapped += QLatin1Char('\n');
        line = QString(text.at(i));
    }
    wrapped += line;
    return wrapped;
}

static QStringList presentParts(const QStringList &parts)
{
    QStringList present;
    for (const QString &part : parts) {
        if (!part.isEmpty())
            present.append(part);
    }
    return present;
}

QVFileInfoPanel::QVFileInfoPanel(QWidget *parent) : QWidget(parent)
{
    setFocusPolicy(Qt::NoFocus);
    setAutoFillBackground(false);

    formWidget = new QWidget(this);
    formWidget->setAutoFillBackground(false);
    auto *form = new QFormLayout(formWidget);
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);

    nameValue = addRow(form, tr("Name:"));
    typeValue = addRow(form, tr("Type:"));
    locationValue = addRow(form, tr("Location:"));
    sizeValue = addRow(form, tr("Size:"));
    modifiedValue = addRow(form, tr("Modified:"));
    dimensionsValue = addRow(form, tr("Dimensions:"));
    ratioValue = addRow(form, tr("Aspect Ratio:"));
    framesValue = addRow(form, tr("Frames:"));
    framesKey = qobject_cast<QLabel *>(form->labelForField(framesValue));

    scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setWidget(formWidget);
    scrollArea->setAutoFillBackground(false);
    scrollArea->viewport()->setAutoFillBackground(false);
    scrollArea->setFocusPolicy(Qt::NoFocus);

    summaryLabel = new QLabel(this);
    summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summaryLabel->setCursor(Qt::IBeamCursor);
    summaryLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    summaryLabel->setAutoFillBackground(false);
    summaryLabel->hide();

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(0);
    layout->addWidget(scrollArea);
    layout->addWidget(summaryLabel);

    settingsUpdated();
    applyChrome();
}

QLabel *QVFileInfoPanel::addRow(QFormLayout *form, const QString &labelText)
{
    auto *key = new QLabel(labelText, formWidget);
    key->setAlignment(Qt::AlignRight | Qt::AlignTop);
    keys.append(key);
    auto *value = new QLabel(formWidget);
    value->setWordWrap(true);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    value->setCursor(Qt::IBeamCursor);
    value->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    value->setMinimumWidth(0);
    value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    form->addRow(key, value);
    return value;
}

void QVFileInfoPanel::setInfo(const QFileInfo &fileInfo, int width, int height, int frameCount)
{
    if (!fileInfo.isFile())
        fields = QVFileInfoFields();
    else
        fields = QVInfoDialog::buildFields(fileInfo, width, height, frameCount);
    updateText();
}

void QVFileInfoPanel::settingsUpdated()
{
    auto &settingsManager = qvApp->getSettingsManager();
    customBackgroundColor = settingsManager.getBool(SettingsManager::Setting::BgColorEnabled)
            ? QColor(settingsManager.getString(SettingsManager::Setting::BgColor))
            : QColor();
    applyPalette();
    update();
}

void QVFileInfoPanel::setPosition(int position)
{
    const bool nextVertical = position != static_cast<int>(Position::Bottom);
    if (positionSet && nextVertical == vertical)
        return;

    positionSet = true;
    vertical = nextVertical;
    scrollArea->setVisible(vertical);
    summaryLabel->setVisible(!vertical);
    applyChrome();
    updateText();
}

void QVFileInfoPanel::setTitlebarInset(int inset)
{
    inset = qMax(0, inset);
    if (inset == titlebarInset)
        return;

    titlebarInset = inset;
    if (vertical)
        applyChrome();
}

void QVFileInfoPanel::applyChrome()
{
    auto *box = static_cast<QVBoxLayout *>(layout());
    setMinimumSize(0, 0);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);

    if (vertical) {
        box->setContentsMargins(TEXT_MARGIN, TEXT_MARGIN + titlebarInset, TEXT_MARGIN, TEXT_MARGIN);
        setFixedWidth(PANEL_WIDTH);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    } else {
        box->setContentsMargins(TEXT_MARGIN, TEXT_MARGIN, TEXT_MARGIN, TEXT_MARGIN);
        const int height = fontMetrics().lineSpacing() * 2 + TEXT_MARGIN * 2;
        setFixedHeight(height);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
}

void QVFileInfoPanel::applyPalette()
{
    QPalette pal = qvApp->palette();
    const QColor background = customBackgroundColor.isValid()
            ? customBackgroundColor
            : pal.color(QPalette::Window);
    pal.setColor(QPalette::All, QPalette::Window, background);
    pal.setColor(QPalette::All, QPalette::Base, background);
    if (customBackgroundColor.isValid()) {
        const QColor text = QVApplication::getPerceivedBrightness(customBackgroundColor) > 0.5
                ? QColor(Qt::black)
                : QColor(Qt::white);
        pal.setColor(QPalette::All, QPalette::WindowText, text);
        pal.setColor(QPalette::All, QPalette::Text, text);
    }
    setPalette(pal);
    formWidget->setPalette(pal);
    scrollArea->setPalette(pal);
    scrollArea->viewport()->setPalette(pal);
    summaryLabel->setPalette(pal);
}

void QVFileInfoPanel::updateText()
{
    const auto showValue = [](QLabel *label, const QString &text, int width) {
        label->setToolTip(text);
        label->setText(width > 0 ? softWrap(text, label->fontMetrics(), width) : text);
    };

    if (vertical) {
        int keyWidth = 0;
        for (QLabel *key : keys) {
            if (!key->isHidden())
                keyWidth = qMax(keyWidth, key->sizeHint().width());
        }
        const int valueWidth = qMax(48, PANEL_WIDTH - TEXT_MARGIN * 2 - 12 - keyWidth);
        showValue(nameValue, fields.name, valueWidth);
        showValue(typeValue, fields.type, valueWidth);
        showValue(locationValue, fields.location, valueWidth);
        showValue(sizeValue, fields.size, valueWidth);
        showValue(modifiedValue, fields.modified, valueWidth);
        showValue(dimensionsValue, fields.dimensions, valueWidth);
        showValue(ratioValue, fields.ratio, valueWidth);
        showValue(framesValue, fields.frames, valueWidth);
        const bool hasFrames = !fields.frames.isEmpty();
        if (framesKey)
            framesKey->setVisible(hasFrames);
        framesValue->setVisible(hasFrames);
        summaryLabel->setToolTip({});
        return;
    }

    const QString framesText = fields.frames.isEmpty()
            ? QString()
            : tr("%1 frames").arg(fields.frames);
    const QString line1 =
            presentParts({ fields.name, fields.dimensions, fields.size, framesText }).join(
                    QStringLiteral("   ·   "));
    const QString line2 =
            presentParts({ fields.type, fields.ratio, fields.modified, fields.location })
                    .join(QStringLiteral("   ·   "));
    const int available = qMax(summaryLabel->width(), width() - TEXT_MARGIN * 2);
    const QFontMetrics metrics = summaryLabel->fontMetrics();
    const QString shown = metrics.elidedText(line1, Qt::ElideMiddle, available) + QLatin1Char('\n')
            + metrics.elidedText(line2, Qt::ElideMiddle, available);
    summaryLabel->setText(shown);
    summaryLabel->setToolTip(line1 + QLatin1Char('\n') + line2);
}

void QVFileInfoPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (updatingText)
        return;

    updatingText = true;
    updateText();
    updatingText = false;
}

void QVFileInfoPanel::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    const QColor background = customBackgroundColor.isValid()
            ? customBackgroundColor
            : palette().color(QPalette::Window);
    painter.fillRect(rect(), background);

    const QColor separator = QVApplication::getPerceivedBrightness(background) > 0.5
            ? QColor(0, 0, 0, 60)
            : QColor(255, 255, 255, 60);
    painter.setPen(QPen(separator, 1));
    if (vertical)
        painter.drawLine(0, 0, 0, height() - 1);
    else
        painter.drawLine(0, 0, width() - 1, 0);
}
