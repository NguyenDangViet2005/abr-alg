#include <QCoreApplication>
#include "XBQoSService.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    XBQoSService qosService;
    qosService.start();

    return QCoreApplication::exec();
}