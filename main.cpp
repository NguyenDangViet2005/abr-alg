#include <QCoreApplication>
#include <QCommandLineParser>
#include "XBQoSService.h"
#include "ABRConfigs.h"
#include "SRTPeerStat.h"
#include "RouterInterfaceStat.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);
    QCoreApplication::setApplicationName("XBQoSService");
    QCoreApplication::setApplicationVersion("1.0");

    // XBQoSService has NO config file, and one must not be invented: the
    // router owns config.ini and regenerating it is forbidden. CLI args are
    // the smallest mechanism that turns the debug channel on.
    QCommandLineParser cli;
    cli.setApplicationDescription("XBQoS adaptive bitrate service");
    cli.addHelpOption();
    cli.addVersionOption();
    const QCommandLineOption optQosDebug(
        "qos-debug", "Stream QoS debug records (QS format) to the dashboard + file.");
    const QCommandLineOption optHost(
        "qos-debug-host", "Dashboard host.", "host", QOS_DEBUG_DEFAULT_HOST);
    const QCommandLineOption optPort(
        "qos-debug-port", "Dashboard UDP port.", "port",
        QString::number(QOS_DEBUG_DEFAULT_PORT));
    const QCommandLineOption optDir(
        "qos-debug-log-dir", "Directory for .qsdb session files.", "dir",
        QOS_DEBUG_DEFAULT_DIR);
    cli.addOption(optQosDebug);
    cli.addOption(optHost);
    cli.addOption(optPort);
    cli.addOption(optDir);
    cli.process(a);

    qRegisterMetaType<SRTPeerStat>("SRTPeerStat");
    qRegisterMetaType<QVector<SRTPeerStat>>("QVector<SRTPeerStat>");

    // Registered for the same reason as the two above, even though the
    // router-row connections are currently DIRECT (handler, factory and the
    // service all live in this thread). A later moveToThread would turn them
    // queued, and an unregistered type fails at RUNTIME with the emission
    // dropped — not at compile time.
    qRegisterMetaType<RouterInterfaceStat>("RouterInterfaceStat");

    XBQoSService qosService;

    // ── Router ABR interface list ──
    // MUST mirror the router's own `interfaces=` in config.ini. The router
    // ships `interfaces=eth1:1` (see sample-general.ini) — that is the list of
    // names its id-60 rows carry, so it is the list this service must declare.
    // A name declared here but never seen (or seen but not declared) surfaces
    // as the "unknown iface" warning in ABRFactory::handleRouterRow.
    // XBQoSService has no config file of its own; this line IS the config.
    // Adding an interface is one entry here — nothing else changes.
    //
    // isC2Configured mirrors the router's `c2Port=6974` (set = C2 deployed). It
    // is a GLOBAL fact — whether C2 exists at all — NOT "this link's C2 subflow
    // is alive".
    //
    // It is currently INERT: the port is xbfirm's algorithm verbatim, and that
    // algorithm has no C2 branch to gate on — the flag is stored and logged by
    // XBAdaptiveBitrateStreaming, nothing more. (The old C2 4-state machine and
    // decideBitrate, which DID read it, were deleted in Phase 03.) Keep it wired
    // so the topology fact reaches the instance; there is no switch to flip yet.
    constexpr bool C2_DEPLOYED = false;
    qosService.setRouterAbrInterfaces({"eth1"}, C2_DEPLOYED);

    qosService.setQosDebug(cli.isSet(optQosDebug),
                           cli.value(optHost),
                           static_cast<quint16>(cli.value(optPort).toUShort()),
                           cli.value(optDir));

    qosService.start();

    return QCoreApplication::exec();
}
