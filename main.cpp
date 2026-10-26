#include <QCoreApplication>
#include "XBQoSService.h"
#include "SRTPeerStat.h"
#include "SiyiLinkStat.h"
#include "C2TelemetryStat.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    qRegisterMetaType<SRTPeerStat>("SRTPeerStat");
    qRegisterMetaType<QVector<SRTPeerStat>>("QVector<SRTPeerStat>");
    qRegisterMetaType<SiyiLinkStat>("SiyiLinkStat");
    qRegisterMetaType<C2TelemetryStat>("C2TelemetryStat");
    qRegisterMetaType<QVector<C2TelemetryStat>>("QVector<C2TelemetryStat>");

    XBQoSService qosService;
    qosService.start();

    return QCoreApplication::exec();
}
