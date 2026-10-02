#include "XBQoSService.h"
#include <QDebug>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QTimer>
#include "ABRFactory.h"
#include "dev/NetworkHandler.h"
#include "dev/RouterNetworkHandler.h"
#include "dev/CameraControl.h"
#include "dev/QoSDebugSender.h"
#include "ABRConfigs.h"

XBQoSService::XBQoSService(QObject *parent)
    : QObject(parent)
    , m_abrFactory(nullptr)
    , m_networkHandler(nullptr)
    , m_routerNetworkHandler(nullptr)
    , m_cameraControl(nullptr)
    , m_qosDebug(nullptr)
    , m_currentBitrate(0)
    , m_cameraNeedsSync(false)
    , m_cameraKeepAliveTimer(nullptr)
{
    m_cameraControl = new CameraControl(this);
}

XBQoSService::~XBQoSService()
{
    stop();
}

void XBQoSService::setRouterAbrInterfaces(const QStringList &ifaces, bool isC2Configured)
{
    m_routerAbrIfaces = ifaces;
    m_isC2Configured = isC2Configured;
}

void XBQoSService::setQosDebug(bool enabled, const QString &host, quint16 port,
                               const QString &logDir)
{
    m_qosDebugEnabled = enabled;
    m_qosDebugHost = host;
    m_qosDebugPort = port;
    m_qosDebugLogDir = logDir;
}

void XBQoSService::printStartupBanner()
{
    qInfo() << "============================================================";
    qInfo() << "  XB-QOS-SERVICE : BELACODER ADAPTIVE BITRATE (SRT -> RF)   ";
    qInfo() << "============================================================";
}

void XBQoSService::setupConnections()
{
    m_cameraKeepAliveTimer = new QTimer(this);
    m_cameraKeepAliveTimer->setInterval(3000);
    connect(m_cameraKeepAliveTimer, &QTimer::timeout, this, [this]() {
        const bool isTrafficActive = (m_abrFactory && m_abrFactory->srtAbr())
                                         ? m_abrFactory->srtAbr()->isTrafficActive()
                                         : true;
        if (m_currentBitrate > VIDEO_DISABLED_BITRATE_KBPS && isTrafficActive) {
            dispatchToCameraServer(static_cast<int>(m_currentBitrate));
        }
    });

    auto handleBitrateChange = [this](unsigned int newBitrate) {
        if (newBitrate == m_currentBitrate && !m_cameraNeedsSync) {
            return;
        }
        m_currentBitrate = newBitrate;

        if (newBitrate == VIDEO_DISABLED_BITRATE_KBPS) {
            dispatchToCameraServer(VIDEO_DISABLED_BITRATE_KBPS);
            if (m_cameraKeepAliveTimer) {
                m_cameraKeepAliveTimer->stop();
            }
            return;
        }

        VideoProfile profile = m_resolutionAdapter.updateBitrate(newBitrate);
        Q_UNUSED(profile);
        dispatchToCameraServer(static_cast<int>(newBitrate));

        if (m_cameraKeepAliveTimer) {
            m_cameraKeepAliveTimer->start();
        }
    };

    if (m_abrFactory && m_abrFactory->srtAbr()) {
        connect(m_abrFactory->srtAbr(), &IAdaptiveBitrateStreaming::bitrateChanged, this, handleBitrateChange);
        connect(m_abrFactory->srtAbr(), &IAdaptiveBitrateStreaming::trafficActiveChanged, this, [this](bool isActive) {
            if (isActive) {
                if (m_currentBitrate > VIDEO_DISABLED_BITRATE_KBPS) {
                    qInfo().noquote() << QString("[QoS Engine] Video traffic resumed! Resyncing camera to %1 kbps").arg(m_currentBitrate);
                    dispatchToCameraServer(static_cast<int>(m_currentBitrate));
                }
            } else {
                qInfo().noquote() << "[QoS Engine] Video traffic idle / Camera off, pausing keep-alive updates.";
            }
        });
        connect(m_abrFactory->srtAbr(), &IAdaptiveBitrateStreaming::videoStreamEnableChanged, this, [](bool isEnabled) {
            if (!isEnabled) {
                qWarning().noquote() << "[C2 Priority] STRICT_CUTOFF: video disabled";
            } else {
                qInfo().noquote() << "[C2 Normal] Video re-enabled";
            }
        });
        connect(m_abrFactory->srtAbr(), &IAdaptiveBitrateStreaming::c2PriorityChanged, this, [](int level, const QString &name) {
            Q_UNUSED(level);
            if (name.contains("Critical") || name.contains("High")) {
                qWarning().noquote() << QString("[C2 Priority] %1").arg(name);
            }
        });
        connect(m_abrFactory->srtAbr(), &IAdaptiveBitrateStreaming::requestKeyframe, this, [this]() {
            if (m_cameraControl && m_currentBitrate > VIDEO_DISABLED_BITRATE_KBPS) {
                m_cameraControl->requestStreamRefresh(static_cast<int>(m_currentBitrate));
            }
        });
    }

    if (m_abrFactory) {
        connect(m_networkHandler, &NetworkHandler::onQosDataReceived, m_abrFactory, &ABRFactory::onSrtCameraConnection);
        connect(m_networkHandler, &NetworkHandler::onC2DataReceived, m_abrFactory, &ABRFactory::handleC2Data);

        // Router telemetry (UDP 12346). Routed to the per-interface ABR
        // instances (ABRFactory::handleRouterRow → XBAdaptiveBitrateStreaming).
        // Deliberately SEPARATE from the 12345 SRT QoS path below: they
        // describe different things (router's own per-interface view vs.
        // XBSRTFactory's measurement of the SRT stream) and must not be merged.
        connect(m_routerNetworkHandler, &RouterNetworkHandler::onRouterRow,
                m_abrFactory, &ABRFactory::handleRouterRow);
        connect(m_routerNetworkHandler, &RouterNetworkHandler::onRouterTick,
                m_abrFactory, &ABRFactory::handleRouterTick);

        // QoS debug channel taps the SAME signals. It only reads, so the tap
        // order relative to the factory does not matter.
        if (m_qosDebug) {
            connect(m_routerNetworkHandler, &RouterNetworkHandler::onRouterRow,
                    m_qosDebug, &QoSDebugSender::handleRouterRow);
            connect(m_routerNetworkHandler, &RouterNetworkHandler::onRouterTick,
                    m_qosDebug, &QoSDebugSender::handleTick);
            // The ABR decision is computed by the factory, so the channel taps
            // the factory's relay rather than each per-interface instance. This
            // is what fills abrBranch/abrReason/maxAbr in every SAMPLE.
            connect(m_abrFactory, &ABRFactory::routerAbrDecision,
                    m_qosDebug, &QoSDebugSender::handleAbrDecision);
        }
        connect(m_networkHandler, &NetworkHandler::onQosDataReceived, this, [this]() {
            // Khi nhận lại được gói QoS camera (prefix 9990) mà trước đó lệnh gửi camera bị lỗi
            if (m_cameraNeedsSync && m_currentBitrate > VIDEO_DISABLED_BITRATE_KBPS) {
                m_cameraNeedsSync = false;
                qInfo().noquote() << QString("[QoS Engine] Camera stream active, resyncing bitrate to %1 kbps").arg(m_currentBitrate);
                dispatchToCameraServer(static_cast<int>(m_currentBitrate));
            }
        });
    }

    if (m_cameraControl && m_abrFactory && m_abrFactory->srtAbr()) {
        connect(m_cameraControl, &CameraControl::bitrateReported, this, [this](int requestedKbps, int achievedKbps) {
            m_cameraNeedsSync = false;
            if (achievedKbps > 0) {
                if (qAbs(achievedKbps - requestedKbps) > 250) {
                    qWarning().noquote() << QString("[CameraControl] Note: Camera reported bitrate %1 kbps (differs from target %2 kbps)")
                                                .arg(achievedKbps).arg(requestedKbps);
                }
                m_abrFactory->srtAbr()->handleCameraReportedBitrate(achievedKbps);
            }
        });
        connect(m_cameraControl, &CameraControl::bitrateRejected, this, [this](int requestedKbps, const QString &reason) {
            qWarning().noquote() << QString(">>> [QoS] ⚠️ Camera REJECTED command %1 kbps: %2 — ABR will resync upon connection <<<")
                                        .arg(requestedKbps)
                                        .arg(reason);
            m_cameraNeedsSync = true;
        });
    }
}

void XBQoSService::dispatchToCameraServer(int bitrate)
{
    if (m_cameraControl) {
        m_cameraControl->sendAdaptBitrate(bitrate);
    }
}

void XBQoSService::start()
{
    printStartupBanner();

    m_abrFactory = ABRFactory::instance();
    m_abrFactory->init();
    m_abrFactory->startCameraSocketAbr();

    // Per-interface router ABR. Declared list comes from main.cpp (see
    // setRouterAbrInterfaces) — the router's config.ini `interfaces=` is a
    // SEPARATE list and is never regenerated; a mismatch surfaces as the
    // "unknown iface" warning in ABRFactory::handleRouterRow (report risk #5).
    if (!m_routerAbrIfaces.isEmpty()) {
        m_abrFactory->startRouterAbr(m_routerAbrIfaces, m_isC2Configured);
    } else {
        qWarning().noquote() << "[QoS Engine] No router ABR interfaces declared — "
                                "per-interface ABR disabled (router rows will be logged only).";
    }

    m_networkHandler = new NetworkHandler(this);
    m_routerNetworkHandler = new RouterNetworkHandler(this);

    // Created BEFORE setupConnections so the tap connects to a live object.
    // The debug channel is harmless when disabled: it logs but never opens a
    // socket or file.
    m_qosDebug = new QoSDebugSender(this);
    m_qosDebug->setEnabled(m_qosDebugEnabled);

    setupConnections();

    m_networkHandler->start(SRT_ABR_QOS_UDP_PORT);
    qInfo() << "[QoS Engine] Service started. Listening on UDP port" << SRT_ABR_QOS_UDP_PORT;

    // RouterNetworkHandler logs its own bind result (success or errorString),
    // so no service-level "listening" line here to avoid claiming success on a
    // failed bind.
    m_routerNetworkHandler->start(ROUTER_TELEMETRY_UDP_PORT);

    // Activate the debug channel's I/O (if enabled) and announce the topology.
    if (m_qosDebugEnabled) {
        m_qosDebug->startUdp(m_qosDebugHost, m_qosDebugPort);
        m_qosDebug->startFileLog(m_qosDebugLogDir);
        QVector<QPair<QString, quint8>> topo;
        topo.reserve(m_routerAbrIfaces.size());
        for (const QString &name : m_routerAbrIfaces) {
            topo.append({ name, 1 });   // priority filled by the router's own rows
        }
        m_qosDebug->setTopology(topo, QString(), m_qosDebugPort);
        m_qosDebug->emitConfig();
    } else {
        qInfo().noquote() << "[QoS Engine] QoS debug channel disabled "
                             "(qDebug only, no UDP/file). Enable with --qos-debug.";
    }
}

void XBQoSService::stop()
{
    if (m_cameraKeepAliveTimer) {
        m_cameraKeepAliveTimer->stop();
    }
    if (m_networkHandler) {
        m_networkHandler->stop();
    }
    if (m_routerNetworkHandler) {
        m_routerNetworkHandler->stop();
    }
    if (m_qosDebug) {
        m_qosDebug->stopFileLog();
        m_qosDebug->stopUdp();
    }
    if (m_abrFactory) {
        m_abrFactory->stopRouterAbr();
        m_abrFactory->stopCameraSocketAbr();
    }
}