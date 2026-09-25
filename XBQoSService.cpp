#include "XBQoSService.h"
#include <QDebug>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QTimer>
#include "ABRFactory.h"
#include "dev/NetworkHandler.h"
#include "dev/CameraControl.h"
#include "ABRConfigs.h"

XBQoSService::XBQoSService(QObject *parent)
    : QObject(parent)
    , m_abrFactory(nullptr)
    , m_networkHandler(nullptr)
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
    m_cameraKeepAliveTimer->setInterval(3000);
    connect(m_cameraKeepAliveTimer, &QTimer::timeout, this, [this]() {
        if (m_currentBitrate > VIDEO_DISABLED_BITRATE_KBPS) {
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

        qInfo().noquote() << QString(">>> [BITRATE OUTPUT] ===> [%1 kbps] dispatched to Camera <<<").arg(newBitrate);
        dispatchToCameraServer(static_cast<int>(newBitrate));

        if (m_cameraKeepAliveTimer) {
            m_cameraKeepAliveTimer->start();
        }
    };

    if (m_abrFactory && m_abrFactory->srtAbr()) {
        connect(m_abrFactory->srtAbr(), &IAdaptiveBitrateStreaming::bitrateChanged, this, handleBitrateChange);
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
    } else if (m_abrFactory) {
        connect(m_abrFactory, &ABRFactory::onCamSockBitrateChanged, this, handleBitrateChange);
    }

    if (m_abrFactory) {
        connect(m_networkHandler, &NetworkHandler::onQosDataReceived, m_abrFactory, &ABRFactory::onSrtCameraConnection);
        connect(m_networkHandler, &NetworkHandler::onC2DataReceived, m_abrFactory, &ABRFactory::handleC2Data);
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

    setupConnections();

    m_networkHandler->start(SRT_ABR_QOS_UDP_PORT);
    qInfo() << "[QoS Engine] Service started. Listening on UDP port" << SRT_ABR_QOS_UDP_PORT;
}

void XBQoSService::stop()
{
    if (m_cameraKeepAliveTimer) {
        m_cameraKeepAliveTimer->stop();
    }
    if (m_networkHandler) {
        m_networkHandler->stop();
    }
}