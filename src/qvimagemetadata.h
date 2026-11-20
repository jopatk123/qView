#ifndef QVIMAGEMETADATA_H
#define QVIMAGEMETADATA_H

#include <QString>

class QVImageMetadata
{
public:
    struct Fields
    {
        QString taken;
        QString camera;
        QString exposure;
        QString gps;
    };

    static Fields read(const QString &filePath);
};

#endif // QVIMAGEMETADATA_H
