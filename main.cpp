#include <QCoreApplication>
#include "XBQoSService.h"
#include "SRTPeerStat.h"
#include "SiyiLinkStat.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    qRegisterMetaType<SRTPeerStat>("SRTPeerStat");
    qRegisterMetaType<QVector<SRTPeerStat>>("QVector<SRTPeerStat>");
    qRegisterMetaType<SiyiLinkStat>("SiyiLinkStat");

    XBQoSService qosService;
    qosService.start();

    return QCoreApplication::exec();
}
