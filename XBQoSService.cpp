#include "XBQoSService.h"
#include <QDebug>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QTimer>
#include "ABRFactory.h"
#include "dev/NetworkHandler.h"
#include "dev/CameraControl.h"
#include "dev/SiyiLinkHandler.h"
#include "dev/XbTelemetryHandler.h"
#include "ABRConfigs.h"

XBQoSService::XBQoSService(QObject *parent)
    : QObject(parent)
    , m_abrFactory(nullptr)
    , m_networkHandler(nullptr)
    , m_siyiHandler(nullptr)
    , m_telemetryHandler(nullptr)
    , m_cameraControl(nullptr)
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

void XBQoSService::printStartupBanner()
{
    qInfo() << "============================================================";
    qInfo() << "  XB-QOS-SERVICE : BELACODER ADAPTIVE BITRATE (SRT -> RF)   ";
    qInfo() << "============================================================";
}

void XBQoSService::setupConnections()
{
    m_cameraKeepAliveTimer = new QTimer(this);
    m_cameraKeepAliveTimer->setInterval(5000);
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

        m_resolutionAdapter.updateBitrate(newBitrate);
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
            if (name.contains("Critical", Qt::CaseInsensitive) || name.contains("High", Qt::CaseInsensitive)) {
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
        m_abrFactory->srtAbr()->setNativeC2TelemetryEnabled(m_telemetryHandler != nullptr);
        if (m_telemetryHandler) {
            connect(m_telemetryHandler, &XbTelemetryHandler::telemetryUpdated, m_abrFactory, &ABRFactory::handleNativeC2Data);
            connect(m_telemetryHandler, &XbTelemetryHandler::captureError, this, [](const QString &message) {
                qWarning().noquote() << "[C2]" << message;
            });
        }
        connect(m_networkHandler, &NetworkHandler::onQosDataReceived, m_abrFactory, &ABRFactory::onSrtCameraConnection);
        connect(m_networkHandler, &NetworkHandler::onC2DataReceived, m_abrFactory, &ABRFactory::handleC2Data);
        if (m_siyiHandler) {
            connect(m_siyiHandler, &SiyiLinkHandler::onSiyiLinkUpdated, m_abrFactory, &ABRFactory::handleSiyiData);
            connect(m_siyiHandler, &SiyiLinkHandler::onSiyiLinkLost, m_abrFactory, &ABRFactory::handleSiyiLost);
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

    m_networkHandler = new NetworkHandler(this);
    m_siyiHandler = new SiyiLinkHandler(this);
    // Opt out explicitly for deployments still using legacy 9991 C2 stats.
    if (qgetenv("XB_C2_TELEMETRY") != "0") m_telemetryHandler = new XbTelemetryHandler(this);

    setupConnections();

    m_networkHandler->start(SRT_ABR_QOS_UDP_PORT);
    m_siyiHandler->start();
    if (m_telemetryHandler) {
        const QString interface = qEnvironmentVariable("XB_C2_INTERFACE", C2_TELEMETRY_INTERFACE);
        m_telemetryHandler->start(interface);
    }

    qInfo() << "[QoS Engine] Service started. Listening on UDP port" << SRT_ABR_QOS_UDP_PORT;
    qInfo().noquote() << QString("[QoS Engine] SIYI HM30 Handler started. Polling AU at %1:%2 (Local bind %3)")
                                .arg(SIYI_AU_DEFAULT_IP)
                                .arg(SIYI_AU_DEFAULT_PORT)
                                .arg(SIYI_LOCAL_DEFAULT_PORT);
}

void XBQoSService::stop()
{
    if (m_cameraKeepAliveTimer) {
        m_cameraKeepAliveTimer->stop();
    }
    if (m_networkHandler) {
        m_networkHandler->stop();
    }
    if (m_siyiHandler) {
        m_siyiHandler->stop();
    }
    if (m_telemetryHandler) m_telemetryHandler->stop();
    if (m_abrFactory) {
        m_abrFactory->stopCameraSocketAbr();
    }
}
