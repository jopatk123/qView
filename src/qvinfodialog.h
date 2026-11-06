#ifndef QVINFODIALOG_H
#define QVINFODIALOG_H

#include <QDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLocale>
#include <QString>

struct QVFileInfoFields
{
    QString name;
    QString type;
    QString location;
    QString size;
    QString modified;
    QString dimensions;
    QString ratio;
    QString frames;
    QString taken;
    QString camera;
    QString exposure;
    QString gps;
};

namespace Ui {
class QVInfoDialog;
}

class QVInfoDialog : public QDialog
{
    Q_OBJECT

public:
    explicit QVInfoDialog(QWidget *parent = nullptr);
    ~QVInfoDialog();

    void setInfo(const QFileInfo &value, const int &value2, const int &value3, const int &value4);

    void updateInfo();

    static QVFileInfoFields buildFields(const QFileInfo &fileInfo, int width, int height,
                                        int frameCount);

private:
    Ui::QVInfoDialog *ui;

    QFileInfo selectedFileInfo;
    int width;
    int height;

    int frameCount;

    QLabel *takenKey = nullptr;
    QLabel *takenValue = nullptr;
    QLabel *cameraKey = nullptr;
    QLabel *cameraValue = nullptr;
    QLabel *exposureKey = nullptr;
    QLabel *exposureValue = nullptr;
    QLabel *gpsKey = nullptr;
    QLabel *gpsValue = nullptr;

public:
    // If Qt 5.10 is available, the built-in function will be used--for Qt 5.9, a custom solution
    // will be used
    static QString formatBytes(qint64 bytes)
    {
        QString sizeString;
#if (QT_VERSION >= QT_VERSION_CHECK(5, 10, 0))
        QLocale locale;
        sizeString = locale.formattedDataSize(bytes);
#else
        double size = bytes;

        int reductionAmount = 0;
        double newSize = size / 1024;
        while (newSize > 1024) {
            newSize /= 1024;
            reductionAmount++;
            if (reductionAmount > 2)
                break;
        }

        QString unit;
        switch (reductionAmount) {
        case 0: {
            unit = " KiB";
            break;
        }
        case 1: {
            unit = " MiB";
            break;
        }
        case 2: {
            unit = " GiB";
            break;
        }
        case 3: {
            unit = " TiB";
            break;
        }
        }

        sizeString = QString::number(newSize, 'f', 2) + unit;
#endif
        return sizeString;
    }
};

#endif // QVINFODIALOG_H
