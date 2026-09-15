#include <QCoreApplication>
#include "XBQoSService.h"
#include "SRTPeerStat.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    qRegisterMetaType<SRTPeerStat>("SRTPeerStat");
    qRegisterMetaType<QVector<SRTPeerStat>>("QVector<SRTPeerStat>");

    XBQoSService qosService;
    qosService.start();

    return QCoreApplication::exec();
}
