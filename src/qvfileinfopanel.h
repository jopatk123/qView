#ifndef QVFILEINFOPANEL_H
#define QVFILEINFOPANEL_H

#include "qvinfodialog.h"

#include <QList>
#include <QWidget>

class QFormLayout;
class QLabel;
class QScrollArea;

class QVFileInfoPanel : public QWidget
{
    Q_OBJECT
public:
    // Positions must match the FileInfoPanelPosition setting
    enum class Position
    {
        Hidden = 0,
        Right = 1,
        Bottom = 2
    };

    explicit QVFileInfoPanel(QWidget *parent = nullptr);

    void setInfo(const QFileInfo &fileInfo, int width, int height, int frameCount);

    // Re-read the window background color
    void settingsUpdated();

    // 1 = right (vertical), 2 = bottom (horizontal strip)
    void setPosition(int position);

    // Pixels of the macOS titlebar that cover the top of a vertical panel
    void setTitlebarInset(int inset);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QLabel *addRow(QFormLayout *form, const QString &labelText);
    void applyChrome();
    void applyPalette();
    void updateText();

    QScrollArea *scrollArea;
    QWidget *formWidget;
    QLabel *summaryLabel;

    QLabel *nameValue;
    QLabel *typeValue;
    QLabel *locationValue;
    QLabel *sizeValue;
    QLabel *modifiedValue;
    QLabel *dimensionsValue;
    QLabel *ratioValue;
    QLabel *framesKey;
    QLabel *framesValue;
    QList<QLabel *> keys;

    QVFileInfoFields fields;
    QColor customBackgroundColor;
    int titlebarInset = 0;
    bool vertical = true;
    bool positionSet = false;
    bool updatingText = false;

    static const int PANEL_WIDTH = 260;
    static const int TEXT_MARGIN = 10;
};

#endif // QVFILEINFOPANEL_H
