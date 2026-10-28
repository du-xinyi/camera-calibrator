#include "mainwindow.h"
#include "theme.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("camera-calibrator");
    Theme::apply(app);

    MainWindow window;
    window.show();

    return app.exec();
}
