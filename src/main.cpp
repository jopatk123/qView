#include "mainwindow.h"
#include "qvapplication.h"
#include "qvwin32functions.h"

#include <QCommandLineParser>
#include <QTimer>
#include <QCoreApplication>
#include <cstdlib>

int main(int argc, char *argv[])
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
    QCoreApplication::setOrganizationName("qView");
    QCoreApplication::setApplicationName("qView");
    QCoreApplication::setApplicationVersion(QString::number(VERSION));
    QVApplication app(argc, argv);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QObject::tr("file"), QObject::tr("The file to open."));
#if defined Q_OS_WIN && WIN32_LOADED && QT_VERSION < QT_VERSION_CHECK(6, 7, 2)
    // Workaround for unicode characters getting mangled in certain cases. To support unicode
    // arguments on Windows, QCoreApplication normally ignores argv and gets them from the Windows
    // API instead. But this only happens if it thinks argv hasn't been modified prior to being
    // passed into QCoreApplication's constructor. Certain characters like U+2033 (double prime) get
    // converted differently in argv versus the value Qt is comparing with (__argv). This makes Qt
    // incorrectly think the data was changed, and it skips fetching unicode arguments from the API.
    // https://bugreports.qt.io/browse/QTBUG-125380
    parser.process(QVWin32Functions::getCommandLineArgs());
#else
    parser.process(app);
#endif

    auto *window = QVApplication::newWindow();

    if (qEnvironmentVariableIsSet("QV_CAF_SELFTEST")) {
        const QString path = parser.positionalArguments().isEmpty()
                ? QString::fromUtf8(qgetenv("QV_CAF_SELFTEST"))
                : parser.positionalArguments().constFirst();
        if (path.isEmpty() || path == QLatin1String("1")) {
            qWarning("QV_CAF_SELFTEST set but no image path (pass file arg or QV_CAF_SELFTEST=/path.png)");
            return 2;
        }
        window->show();
        auto *exitCode = new int(1);
        QTimer::singleShot(300, window, [window, path, exitCode]() {
            *exitCode = window->runCafSelfTest(path);
            QCoreApplication::exit(*exitCode);
        });
        const int rc = QApplication::exec();
        const int out = *exitCode;
        delete exitCode;
        return out != 0 ? out : rc;
    }

    if (!parser.positionalArguments().isEmpty())
        QVApplication::openFile(window, parser.positionalArguments().constFirst(), true);

    return QApplication::exec();
}
