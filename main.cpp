#include <QCoreApplication>
#include "XBQoSService.h"
#include "SRTPeerStat.h"
#include "RouterInterfaceStat.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    qRegisterMetaType<SRTPeerStat>("SRTPeerStat");
    qRegisterMetaType<QVector<SRTPeerStat>>("QVector<SRTPeerStat>");

    // Registered for the same reason as the two above, even though the
    // router-row connections are currently DIRECT (handler, factory and the
    // service all live in this thread). A later moveToThread would turn them
    // queued, and an unregistered type fails at RUNTIME with the emission
    // dropped — not at compile time.
    qRegisterMetaType<RouterInterfaceStat>("RouterInterfaceStat");

    XBQoSService qosService;
    qosService.start();

    return QCoreApplication::exec();
}
