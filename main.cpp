#include <QCoreApplication>
#include <QCommandLineParser>
#include "XBQoSService.h"
#include "ABRConfigs.h"
#include "SRTPeerStat.h"
#include "RouterInterfaceStat.h"
#include "dev/LogUploader.h"

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

    // Phase 09 (headroom): the router ABR's steady-state cap, as a PERCENT of the
    // learned ceiling (or of maxAbr when none is learned). Default 80 = reserve
    // 20% of the link for other tasks. Clamped to [50,100] by the ABR instance.
    const QCommandLineOption optHeadroom(
        "abr-headroom", "Router-ABR steady-state cap as a percent of the ceiling (50-100).",
        "pct", QString::number(ABR_HEADROOM_FACTOR_PCT));
    cli.addOption(optHeadroom);

    // --disable-abr: freeze the camera bitrate. The ABR instances keep running
    // and keep logging (shadow mode), and the dashboard keeps receiving their
    // decisions — only the OUTBOUND commands are muted. For A/B measurement:
    // compare "ABR in control" against "camera on its own default" with the
    // decision stream still visible. Socket restarts (loss > 45% / score cliff)
    // remain enabled on purpose — see XBQoSService::setAbrDisabled.
    const QCommandLineOption optDisableAbr(
        "disable-abr", "Do not change the camera bitrate. The ABR still runs and logs.");
    cli.addOption(optDisableAbr);

    // One-shot log upload. When set, the service does NOT start: it finds the
    // newest .qsdb in --qos-debug-log-dir, POSTs it to the given host:port, and
    // exits. The log dir is REUSED (not a separate flag) — the same directory
    // QoSDebugSender writes to, so the two stay in sync by construction.
    const QCommandLineOption optUpload(
        "upload", "Upload the latest .qsdb log to <host>:<port>, then exit.",
        "host:port");
    cli.addOption(optUpload);
    cli.process(a);

    // ── One-shot upload mode ──
    // Runs BEFORE any service construction: no ABR factory, no UDP listeners,
    // no camera socket. The event loop only runs to drive the HTTP reply.
    if (cli.isSet(optUpload)) {
        LogUploader uploader;
        QObject::connect(&uploader, &LogUploader::finished,
                         &a, [](bool ok, const QString &msg) {
            if (ok) {
                qInfo().noquote() << QString("[LogUpload] OK: %1").arg(msg);
            } else {
                qWarning().noquote() << QString("[LogUpload] FAILED: %1").arg(msg);
            }
            QCoreApplication::exit(ok ? 0 : 1);
        });
        uploader.uploadLatest(cli.value(optDir), cli.value(optUpload));
        return QCoreApplication::exec();
    }

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
    // ships `interfaces=eth1:1,eth2:1` (sample-general.ini, commit bfda34b:
    // dual-interface MPTCP) — that is the list of names its id-60 rows carry,
    // so it is the list this service must declare. Declaring only "eth1" drops
    // every eth2 row before the ABR sees it ("unknown iface" warning), so
    // eth2 publishes br=0 / max=0 / ceil=0 / cap=0 on the dashboard — it has no
    // ABR instance to compute a bitrate. A name declared here but never seen (or
    // seen but not declared) surfaces as the "unknown iface" warning in
    // ABRFactory::handleRouterRow.
    // "wlan0" added 2026-10-16: the router now also publishes a WiFi-STA row
    // (OpenWrt box on Spectrum_AP_Demo), so it needs an ABR instance too —
    // otherwise its rows are dropped as "unknown iface". The router-side
    // `interfaces=` must include wlan0 for this to have any effect.
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
    qosService.setRouterAbrInterfaces({"eth1", "eth2", "wlan0"}, C2_DEPLOYED);

    // Phase 09 (headroom): apply the CLI override (or the #define default).
    qosService.setAbrHeadroomPct(cli.value(optHeadroom).toInt());

    // --disable-abr: mute the outbound bitrate commands (ABR keeps running).
    qosService.setAbrDisabled(cli.isSet(optDisableAbr));

    qosService.setQosDebug(cli.isSet(optQosDebug),
                           cli.value(optHost),
                           static_cast<quint16>(cli.value(optPort).toUShort()),
                           cli.value(optDir));

    qosService.start();

    return QCoreApplication::exec();
}
