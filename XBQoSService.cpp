#include "XBQoSService.h"
#include <QDebug>
#include <QProcess>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QUdpSocket>
#include <QJsonObject>
#include <QJsonDocument>
#include <QHostAddress>
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
    , m_camControlSocket(nullptr)
    , m_isVideoStreamEnabled(true)
{
    m_camControlSocket = new QUdpSocket(this);
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
        if (!m_isVideoStreamEnabled) {
            qInfo() << "[QoS Engine] Video is currently DISABLED (C2 ONLY Mode). Ignoring bitrate update:" << newBitrate << "kbps";
            return;
        }

        VideoProfile profile = m_resolutionAdapter.updateBitrate(newBitrate);

        qInfo() << QString(">>> [Dispatch to Camera Encoder] Bitrate: %1 kbps | Profile: %2 (%3x%4 @ %5fps, Scale: %6%) <<<")
                   .arg(newBitrate)
                   .arg(profile.label)
                   .arg(profile.width)
                   .arg(profile.height)
                   .arg(profile.fps)
                   .arg(profile.scalePercent);

        if (m_aiCompressor) {
            m_aiCompressor->handleChangeBitrate(static_cast<int>(newBitrate));
            m_aiCompressor->handleChangeScale(profile.scalePercent);
            m_aiCompressor->handleChangeFps(profile.fps);
        }

        // Bắn lệnh UDP 5005 sang cam_server.py để điều chỉnh trực tiếp luồng camera HTTP 8888
        sendCameraControlCommand(static_cast<int>(newBitrate), profile, true);
    });

    // Kết nối nhận dữ liệu QoS từ NetworkHandler sang ABRFactory
    connect(m_networkHandler, &NetworkHandler::onQosDataReceived, m_abrFactory, &ABRFactory::onSrtCameraConnection);
    connect(m_networkHandler, &NetworkHandler::onC2DataReceived, m_abrFactory, &ABRFactory::handleC2Data);
    connect(m_networkHandler, &NetworkHandler::onConnectionStateChanged, m_abrFactory, &ABRFactory::handleSerialStatus);

    // Lắng nghe sự kiện Bật/Tắt Video Stream do C2 Priority điều phối
    connect(m_abrFactory, &ABRFactory::onVideoStreamEnableChanged, this, [this](bool isEnabled) {
        m_isVideoStreamEnabled = isEnabled;
        if (!isEnabled) {
            qCritical() << ">>> [C2 SAFETY PROTOCOL TRIGGERED] Video Stream is DISABLED to protect Drone Control Link (C2 ONLY Mode)! <<<";
            // 1. Gửi lệnh UDP 5005 báo cam_server.py ngắt luồng video và hiện màn hình đỏ C2
            sendCameraControlCommand(0, VideoResolutionAdapter::profileOff(), false);
            // 2. Tắt tiến trình phát video camera
            stopCameraStreamer();
            // 3. Tắt bộ nén AICompressor / Camera Encoder
            if (m_aiCompressor) {
                m_aiCompressor->handleChangeBitrate(0);
                m_aiCompressor->handleChangeScale(0);
                m_aiCompressor->handleChangeFps(0);
            }
        } else {
            qInfo() << ">>> [C2 RECOVERY PROTOCOL] Video Stream is re-ENABLED! Resuming adaptive streaming... <<<";
            // 1. Khởi động lại tiến trình phát video camera
            startCameraStreamer();
            // 2. Phục hồi cấu hình video an toàn
            VideoProfile profile = m_resolutionAdapter.currentProfile();
            sendCameraControlCommand(500, profile, true);
            if (m_aiCompressor) {
                m_aiCompressor->handleChangeBitrate(500); // Khởi động ở mức sàn an toàn
                m_aiCompressor->handleChangeScale(profile.scalePercent);
                m_aiCompressor->handleChangeFps(profile.fps);
            }
        }
    });

    // Lắng nghe sự kiện thay đổi C2 Priority Level
    connect(m_abrFactory, &ABRFactory::onC2PriorityChanged, this, [](int level, const QString &name) {
        Q_UNUSED(level);
        qInfo() << ">>> [C2 Priority Level]:" << name << "<<<";
    });
}

void XBQoSService::sendCameraControlCommand(int bitrate, const VideoProfile &profile, bool enabled)
{
    if (!m_camControlSocket) return;

    QJsonObject obj;
    obj["bitrate"] = bitrate;
    obj["width"] = profile.width;
    obj["height"] = profile.height;
    obj["fps"] = profile.fps;
    obj["scale"] = profile.scalePercent;
    obj["enabled"] = enabled;
    obj["label"] = profile.label;

    QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    m_camControlSocket->writeDatagram(data, QHostAddress::LocalHost, 5005);
}

void XBQoSService::startCameraStreamer()
{
    // Không khởi động nếu đang ở chế độ C2_ONLY
    if (!m_isVideoStreamEnabled) {
        qWarning() << "[XBQoSService] Cannot start camera streamer: Video is DISABLED (C2_ONLY mode).";
        return;
    }

    if (m_cameraProcess && m_cameraProcess->state() != QProcess::NotRunning) {
        qInfo() << "[XBQoSService] Camera streamer process is already running.";
        return;
    }

    // Kiểm tra xem có script start_camera.sh trong thư mục ứng dụng, thư mục cha (nếu chạy từ build/), hoặc thư mục làm việc không
    QString appDir = QCoreApplication::applicationDirPath();
    QString scriptPath = appDir + "/start_camera.sh";
    if (!QFile::exists(scriptPath)) {
        scriptPath = appDir + "/../start_camera.sh";
    }
    if (!QFile::exists(scriptPath)) {
        scriptPath = "./start_camera.sh";
    }
    if (!QFile::exists(scriptPath)) {
        scriptPath = "../start_camera.sh";
    }

    if (QFile::exists(scriptPath)) {
        QFileInfo scriptInfo(scriptPath);
        QString workingDir = scriptInfo.absolutePath();
        qInfo() << "[XBQoSService] Found custom camera script:" << scriptInfo.absoluteFilePath() << "- Launching camera stream in" << workingDir;
        if (!m_cameraProcess) {
            m_cameraProcess = new QProcess(this);
            connect(m_cameraProcess, &QProcess::readyReadStandardOutput, this, [this]() {
                QByteArray out = m_cameraProcess->readAllStandardOutput().trimmed();
                if (!out.isEmpty()) qInfo() << "[CameraStream]" << out;
            });
            connect(m_cameraProcess, &QProcess::readyReadStandardError, this, [this]() {
                QByteArray err = m_cameraProcess->readAllStandardError().trimmed();
                if (!err.isEmpty()) qWarning() << "[CameraStream]" << err;
            });
        }
        m_cameraProcess->setWorkingDirectory(workingDir);
        m_cameraProcess->start("/bin/bash", QStringList() << scriptInfo.absoluteFilePath());
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
#ifdef Q_OS_LINUX
    // Đảm bảo kill sạch cả script python streaming hoặc gst nếu chạy độc lập
    QProcess::execute("pkill", QStringList() << "-f" << "cam_server.py");
    QProcess::execute("pkill", QStringList() << "-f" << "gst-launch-1.0");
#endif
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