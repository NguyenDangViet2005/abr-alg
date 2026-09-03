#include "XBQoSService.h"
#include <QDebug>
#include "ABRFactory.h"
#include "AICompressor.h"
#include "dev/NetworkHandler.h"
#include "ABRConfigs.h"

XBQoSService::XBQoSService(QObject *parent)
    : QObject(parent)
    , m_abrFactory(nullptr)
    , m_aiCompressor(nullptr)
    , m_networkHandler(nullptr)
{
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
    // Kết nối signal thay đổi bitrate từ SRT ABR sang AICompressor để điều khiển camera encoder
    connect(m_abrFactory, &ABRFactory::onCamSrtBitrateChanged, this, [this](unsigned int newBitrate) {
        qInfo() << ">>> [Dispatch to Camera Encoder] New Target Bitrate:" << newBitrate << "kbps <<<";
        if (m_aiCompressor) {
            m_aiCompressor->handleChangeBitrate(static_cast<int>(newBitrate));
        }
    });

    // Kết nối nhận dữ liệu QoS từ NetworkHandler sang ABRFactory
    connect(m_networkHandler, &NetworkHandler::onQosDataReceived, m_abrFactory, &ABRFactory::onSrtCameraConnection);
    connect(m_networkHandler, &NetworkHandler::onConnectionStateChanged, m_abrFactory, &ABRFactory::handleSerialStatus);
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

    // 3. Khởi tạo NetworkHandler kết nối UDP tới QoS Server (Mặc định Port 12345)
    m_networkHandler = new NetworkHandler(this);

    // 4. Thiết lập kết nối Signal / Slot
    setupConnections();

    // 5. Bắt đầu polling QoS Server
    m_networkHandler->start(QOS_SERVER_DEFAULT_HOST, QOS_SERVER_DEFAULT_PORT, QOS_SERVER_POLL_INTERVAL_MS);
}

void XBQoSService::stop()
{
    if (m_networkHandler) {
        m_networkHandler->stop();
    }
}