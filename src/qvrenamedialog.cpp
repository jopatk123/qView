#include "qvrenamedialog.h"

QVRenameDialog::QVRenameDialog(QWidget *parent, const QString &fileName) : QInputDialog(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);

    setWindowTitle(tr("Rename..."));
    setLabelText(tr("File name:"));
    setTextValue(fileName);
    resize(350, height());

    connect(this, &QDialog::accepted, this,
            [this]() { emit renameRequested(textValue().trimmed()); });
}

void QVRenameDialog::showEvent(QShowEvent *event)
{
    QInputDialog::showEvent(event);

    QLineEdit *lineEdit = findChild<QLineEdit *>();
    const auto &lastDot = lineEdit->text().lastIndexOf(".");
    if (lastDot != -1)
        lineEdit->setSelection(0, lastDot);
}
