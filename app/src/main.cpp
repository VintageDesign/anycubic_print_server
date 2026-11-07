#include "MainWindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("anycubic_print_server"));
    QApplication::setApplicationName(QStringLiteral("PrintMonitor"));

    qRegisterMetaType<QList<ApiClient::FileEntry>>();

    MainWindow w;
    w.show();
    return app.exec();
}
