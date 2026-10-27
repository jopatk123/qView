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
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QLabel *addRow(QFormLayout *form, const QString &labelText);
    void applyChrome();
    void applyPalette();
    void updateText();
    void beginNameEdit();
    void finishNameEdit(bool commit);
    void positionNameEditor();

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
    QList<QLabel *> keys;

    QVFileInfoFields fields;
    QFileInfo currentFileInfo;
    QColor customBackgroundColor;
    int titlebarInset = 0;
    bool vertical = true;
    bool positionSet = false;
    bool updatingText = false;
    bool editingName = false;

    static const int PANEL_WIDTH = 260;
    static const int TEXT_MARGIN = 10;
};

#endif // QVFILEINFOPANEL_H
