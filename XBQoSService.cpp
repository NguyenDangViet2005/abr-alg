#include "XBQoSService.h"
#include <QDebug>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QTimer>
#include "ABRFactory.h"
#include "AICompressor.h"
#include "dev/NetworkHandler.h"
#include "dev/CameraControl.h"
#include "ABRConfigs.h"

XBQoSService::XBQoSService(QObject *parent)
    : QObject(parent)
    , m_abrFactory(nullptr)
    , m_aiCompressor(nullptr)
    , m_networkHandler(nullptr)
    , m_cameraControl(nullptr)
    , m_lastDispatchedScale(-1)
    , m_lastDispatchedFps(-1)
    , m_currentBitrate(0)
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

    // Kết nối signal thay đổi bitrate từ SRT ABR sang Camera Encoder
    auto handleBitrateChange = [this](unsigned int newBitrate) {
        if (newBitrate == m_currentBitrate) {
            return;
        }
        const bool wasDisabled = (m_currentBitrate == VIDEO_DISABLED_BITRATE_KBPS);
        m_currentBitrate = newBitrate;

        if (newBitrate == VIDEO_DISABLED_BITRATE_KBPS) {
            qInfo().noquote() << "\033[1;31m>>> [BITRATE OUTPUT] ===> [VIDEO DISABLED - 0 kbps] <===\033[0m";
            if (m_aiCompressor) {
                m_aiCompressor->handleChangeBitrate(VIDEO_DISABLED_BITRATE_KBPS);
            }
            dispatchToCameraServer(VIDEO_DISABLED_BITRATE_KBPS);
            if (m_cameraKeepAliveTimer) {
                m_cameraKeepAliveTimer->stop();
            }
            return;
        }

        VideoProfile profile = m_resolutionAdapter.updateBitrate(newBitrate);

        qInfo().noquote() << QString("\033[1;32m>>> [BITRATE OUTPUT] ===> [%1 kbps] <===\033[0m | Profile: \033[1;36m%2 (%3x%4 @%5fps - Scale %6%)\033[0m <<<")
                   .arg(newBitrate)
                   .arg(profile.label)
                   .arg(profile.width)
                   .arg(profile.height)
                   .arg(profile.fps)
                   .arg(profile.scalePercent);

        // 1. Cập nhật AICompressor: điều chỉnh Bitrate, Scale và FPS theo nấc phân giải
        if (m_aiCompressor) {
            m_aiCompressor->handleChangeBitrate(static_cast<int>(newBitrate));
            if (wasDisabled || profile.scalePercent != m_lastDispatchedScale) {
                m_lastDispatchedScale = profile.scalePercent;
                m_aiCompressor->handleChangeScale(profile.scalePercent);
            }
            if (wasDisabled || profile.fps != m_lastDispatchedFps) {
                m_lastDispatchedFps = profile.fps;
                m_aiCompressor->handleChangeFps(profile.fps);
            }
        }

        // 2. Call HTTP REST API tới Camera Server thực tế (POST http://host:port/api/camera/adapt-bitrate)
        dispatchToCameraServer(static_cast<int>(newBitrate));

        // Reset lại timer 3s để bắt đầu chu kỳ keepalive từ thời điểm thay đổi mới nhất
        if (m_cameraKeepAliveTimer) {
            m_cameraKeepAliveTimer->start();
        }
    };

    // 1. Kết nối trực tiếp từ SRT Adaptive Bitrate Streaming (tránh trùng lặp tín hiệu qua tầng trung gian)
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
    } else if (m_abrFactory) {
        connect(m_abrFactory, &ABRFactory::onCamSockBitrateChanged, this, handleBitrateChange);
    }

    // Kết nối nhận dữ liệu QoS từ NetworkHandler (Lắng nghe UDP Port 12345 từ background service trên xblink)
    if (m_abrFactory) {
        connect(m_networkHandler, &NetworkHandler::onQosDataReceived, m_abrFactory, &ABRFactory::onSrtCameraConnection);
        connect(m_networkHandler, &NetworkHandler::onC2DataReceived, m_abrFactory, &ABRFactory::handleC2Data);
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

    // 1. Khởi tạo ABR Factory
    m_abrFactory = ABRFactory::instance();
    m_abrFactory->init();
    m_abrFactory->startCameraSocketAbr();

    // 2. Khởi tạo Camera Compressor
    m_aiCompressor = AICompressor::instance();

    // 3. Khởi tạo NetworkHandler làm UDP Server trên Port 12345
    m_networkHandler = new NetworkHandler(this);

    // 4. Thiết lập kết nối Signal / Slot
    setupConnections();

    // 5. Bắt đầu lắng nghe UDP datagrams từ Port 12345
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