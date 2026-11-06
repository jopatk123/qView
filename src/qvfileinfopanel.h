#ifndef QVFILEINFOPANEL_H
#define QVFILEINFOPANEL_H

#include "qvinfodialog.h"

#include <QList>
#include <QWidget>

class QFormLayout;
class QLabel;
class QLineEdit;
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

signals:
    // Emitted when the name row is renamed in place, the actual rename is up to the window
    void fileNameEdited(const QString &newFileName);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QLabel *addRow(QFormLayout *form, const QString &labelText);
    void applyChrome();
    void applyPalette();
    void updateText();
    void beginNameEdit();
    void finishNameEdit(bool commit);
    void positionNameEditor();
    bool inResizeHandle(const QPoint &pos) const;
    int clampedWidth(int width) const;
    void applyPanelWidth(int width, bool persist);

    QScrollArea *scrollArea;
    QWidget *formWidget;
    QLabel *summaryLabel;
    QLineEdit *nameEditor;

    QLabel *nameValue;
    QLabel *typeValue;
    QLabel *locationValue;
    QLabel *sizeValue;
    QLabel *modifiedValue;
    QLabel *dimensionsValue;
    QLabel *ratioValue;
    QLabel *framesKey;
    QLabel *framesValue;
    QLabel *takenKey;
    QLabel *takenValue;
    QLabel *cameraKey;
    QLabel *cameraValue;
    QLabel *exposureKey;
    QLabel *exposureValue;
    QLabel *gpsKey;
    QLabel *gpsValue;
    QList<QLabel *> keys;

    QVFileInfoFields fields;
    QFileInfo currentFileInfo;
    QColor customBackgroundColor;
    int titlebarInset = 0;
    int panelWidth = DEFAULT_WIDTH;
    int dragStartX = 0;
    int dragStartWidth = 0;
    bool vertical = true;
    bool positionSet = false;
    bool updatingText = false;
    bool editingName = false;
    bool resizing = false;

    enum {
        DEFAULT_WIDTH = 260,
        MIN_WIDTH = 180,
        MAX_WIDTH = 640,
        HANDLE_WIDTH = 6,
        TEXT_MARGIN = 10
    };
};

#endif // QVFILEINFOPANEL_H
