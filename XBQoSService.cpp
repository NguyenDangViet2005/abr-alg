#include "XBQoSService.h"
#include <QDebug>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
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
    , m_isVideoStreamEnabled(true)
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
    // Kết nối signal thay đổi bitrate từ SRT ABR sang Camera Encoder
    auto handleBitrateChange = [this](unsigned int newBitrate) {
        if (!m_isVideoStreamEnabled) {
            qInfo() << "[QoS Engine] Video is currently DISABLED (C2 ONLY Mode). Ignoring bitrate update:" << newBitrate << "kbps";
            return;
        }

        VideoProfile profile = m_resolutionAdapter.updateBitrate(newBitrate);

        qInfo().noquote() << QString("[Bitrate Output] %1 kbps | Profile: %2 (%3x%4 @%5fps)")
                   .arg(newBitrate)
                   .arg(profile.label)
                   .arg(profile.width)
                   .arg(profile.height)
                   .arg(profile.fps);

        // 1. Cập nhật AICompressor nếu có
        if (m_aiCompressor) {
            m_aiCompressor->handleChangeBitrate(static_cast<int>(newBitrate));
            m_aiCompressor->handleChangeScale(profile.scalePercent);
            m_aiCompressor->handleChangeFps(profile.fps);
        }

        // 2. Call HTTP REST API tới Camera Server thực tế (POST http://host:port/api/camera/adapt-bitrate)
        dispatchToCameraServer(static_cast<int>(newBitrate), profile, true);
    };

    // 1. Kết nối trực tiếp từ SRT Adaptive Bitrate Streaming (tránh trùng lặp tín hiệu qua tầng trung gian)
    if (m_abrFactory && m_abrFactory->srtAbr()) {
        connect(m_abrFactory->srtAbr(), &IAdaptiveBitrateStreaming::bitrateChanged, this, handleBitrateChange);
        connect(m_abrFactory->srtAbr(), &IAdaptiveBitrateStreaming::videoStreamEnableChanged, this, [this](bool isEnabled) {
            m_isVideoStreamEnabled = isEnabled;
            if (!isEnabled) {
                qCritical().noquote() << "[C2 Safety] Video Stream DISABLED -> C2 ONLY Mode!";
                m_resolutionAdapter.updateBitrate(0);
                // Thông báo tới Camera Server tắt luồng video để nhường toàn bộ băng thông cho C2 Drone
                dispatchToCameraServer(0, VideoResolutionAdapter::profileOff(), false);
                if (m_aiCompressor) {
                    m_aiCompressor->handleChangeBitrate(0);
                    m_aiCompressor->handleChangeScale(0);
                    m_aiCompressor->handleChangeFps(0);
                }
            } else {
                qInfo().noquote() << "[C2 Recovery] Video Stream re-ENABLED!";
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

    // Kết nối nhận dữ liệu QoS từ NetworkHandler sang ABRFactory
    connect(m_networkHandler, &NetworkHandler::onQosDataReceived, m_abrFactory, &ABRFactory::onSrtCameraConnection);
    connect(m_networkHandler, &NetworkHandler::onC2DataReceived, m_abrFactory, &ABRFactory::handleC2Data);
<<<<<<< HEAD
    connect(m_networkHandler, &NetworkHandler::onConnectionStateChanged, m_abrFactory, &ABRFactory::handleSerialStatus);
}
=======
>>>>>>> 0b68b3da79ddedf29a5064388cb35446afa0ede8

void XBQoSService::dispatchToCameraServer(int bitrate, const VideoProfile &profile, bool enabled)
{
    Q_UNUSED(profile);
    Q_UNUSED(enabled);
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

<<<<<<< HEAD
    // 5. Bắt đầu lắng nghe UDP datagrams từ Client/GCS/Mikrotik trên Port 12345
    m_networkHandler->start(SRT_ABR_QOS_UDP_PORT);
    qInfo() << "[QoS Engine] Service started. Listening on UDP port" << SRT_ABR_QOS_UDP_PORT
            << "- Waiting for first live QoS packet from Mikrotik to lock real bitrate...";
=======
    // 5. Bắt đầu lắng nghe SRT Debug UDP trên port 12345
    m_networkHandler->start();
>>>>>>> 0b68b3da79ddedf29a5064388cb35446afa0ede8
}

void XBQoSService::stop()
{
    if (m_networkHandler) {
        m_networkHandler->stop();
    }
}