#include "XBQoSService.h"
#include <QDebug>
#include <QProcess>
#include <QCoreApplication>
#include <QFile>
#include "ABRFactory.h"
#include "AICompressor.h"
#include "dev/NetworkHandler.h"
#include "ABRConfigs.h"

XBQoSService::XBQoSService(QObject *parent)
    : QObject(parent)
    , m_abrFactory(nullptr)
    , m_aiCompressor(nullptr)
    , m_networkHandler(nullptr)
    , m_cameraProcess(nullptr)
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
    connect(m_networkHandler, &NetworkHandler::onConnectionStateChanged, m_abrFactory, &ABRFactory::handleSerialStatus);

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

void XBQoSService::startCameraStreamer()
{
    // Kiểm tra xem có script start_camera.sh trong thư mục ứng dụng hoặc thư mục làm việc không
    QString appDir = QCoreApplication::applicationDirPath();
    QString scriptPath = appDir + "/start_camera.sh";
    if (!QFile::exists(scriptPath)) {
        scriptPath = "./start_camera.sh";
    }

    if (QFile::exists(scriptPath)) {
        qInfo() << "[XBQoSService] Found custom camera script:" << scriptPath << "- Launching camera stream...";
        m_cameraProcess = new QProcess(this);
        m_cameraProcess->start("/bin/bash", QStringList() << scriptPath);
        connect(m_cameraProcess, &QProcess::readyReadStandardOutput, this, [this]() {
            QByteArray out = m_cameraProcess->readAllStandardOutput().trimmed();
            if (!out.isEmpty()) qInfo() << "[CameraStream]" << out;
        });
        connect(m_cameraProcess, &QProcess::readyReadStandardError, this, [this]() {
            QByteArray err = m_cameraProcess->readAllStandardError().trimmed();
            if (!err.isEmpty()) qWarning() << "[CameraStream]" << err;
        });
    } else if (QFile::exists("/dev/video0")) {
        qInfo() << "[XBQoSService] Detected USB Camera at /dev/video0.";
        qInfo() << "[XBQoSService] Note: Create 'start_camera.sh' to automatically launch your custom camera pipeline.";
    }
}

void XBQoSService::stopCameraStreamer()
{
    if (m_cameraProcess && m_cameraProcess->state() != QProcess::NotRunning) {
        qInfo() << "[XBQoSService] Stopping camera streamer process...";
        m_cameraProcess->terminate();
        if (!m_cameraProcess->waitForFinished(2000)) {
            m_cameraProcess->kill();
        }
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

    // 5. Bắt đầu lắng nghe UDP datagrams từ Client/GCS trên Port 12345
    m_networkHandler->start(SRT_ABR_QOS_UDP_PORT);

    // 6. Khởi chạy tiến trình Camera Stream (nếu có start_camera.sh)
    startCameraStreamer();
}

void XBQoSService::stop()
{
    stopCameraStreamer();
    if (m_networkHandler) {
        m_networkHandler->stop();
    }
}