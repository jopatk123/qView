#ifndef QVRENAMEDIALOG_H
#define QVRENAMEDIALOG_H

#include <QInputDialog>
#include <QString>

class QVRenameDialog : public QInputDialog
{
    Q_OBJECT
public:
    QVRenameDialog(QWidget *parent, const QString &fileName);

signals:
    // The rename itself is done by the window so both renaming paths share the
    // same validation and reload logic
    void renameRequested(const QString &newFileName);

protected:
    void showEvent(QShowEvent *event) override;
};

#endif // QVRENAMEDIALOG_H
