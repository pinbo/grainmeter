#include <QApplication>
#include <QIcon>
#include "mainwindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Grainmeter");
    QApplication::setOrganizationName("grainmeter");
    QApplication::setWindowIcon(QIcon(":/grainmeter_icon.png"));

    MainWindow window;
    window.show();
    return app.exec();
}
