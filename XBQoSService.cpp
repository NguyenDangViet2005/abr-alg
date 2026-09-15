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
    connect(m_networkHandler, &NetworkHandler::onC2DataReceived, m_abrFactory, &ABRFactory::handleC2Data);

    // Lắng nghe sự kiện Bật/Tắt Video Stream do C2 Priority điều phối
    connect(m_abrFactory, &ABRFactory::onVideoStreamEnableChanged, this, [](bool isEnabled) {
        if (!isEnabled) {
            qCritical() << ">>> [C2 SAFETY PROTOCOL TRIGGERED] Video Stream is DISABLED to protect Drone Control Link (C2 ONLY Mode)! <<<";
        } else {
            qInfo() << ">>> [C2 RECOVERY PROTOCOL] Video Stream is re-ENABLED! Resuming adaptive streaming... <<<";
        }
    });

    // Lắng nghe sự kiện thay đổi C2 Priority Level
    connect(m_abrFactory, &ABRFactory::onC2PriorityChanged, this, [](int level, const QString &name) {
        Q_UNUSED(level);
        qInfo() << ">>> [C2 Priority Level]:" << name << "<<<";
    });
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

    // 5. Bắt đầu lắng nghe SRT Debug UDP trên port 12345
    m_networkHandler->start();
}

void XBQoSService::stop()
{
    if (m_networkHandler) {
        m_networkHandler->stop();
    }
}