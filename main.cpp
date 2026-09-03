#include <QCoreApplication>
#include <QDebug>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include "ABRFactory.h"
#include "AICompressor.h"

#include "dev/NetworkHandler.h"
#include "ABRConfigs.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    qInfo() << "============================================================";
    qInfo() << "  XB-QOS-SERVICE : BELACODER ADAPTIVE BITRATE (SRT -> RF)   ";
    qInfo() << "============================================================";

    // 1. Khởi tạo ABR Factory
    ABRFactory *abr = ABRFactory::instance();
    abr->init();
    abr->startCameraSocketAbr();

    // 2. Kết nối signal thay đổi bitrate từ SRT ABR sang AICompressor để điều khiển camera
    AICompressor *aiCompressor = AICompressor::instance();
    QObject::connect(abr, &ABRFactory::onCamSrtBitrateChanged, [aiCompressor](unsigned int newBitrate) {
        qInfo() << ">>> [Dispatch to Camera Encoder] New Target Bitrate:" << newBitrate << "kbps <<<";
        aiCompressor->handleChangeBitrate(static_cast<int>(newBitrate));
    });

    // 3. Khởi tạo NetworkHandler kết nối UDP tới QoS Server (Mặc định Port 12345)
    // - Khi test: Giao tiếp trực tiếp với smart_qos_server.ps1 (đo stream thật từ stream_sender.ps1)
    // - Khi triển khai: Kết nối tới IP/Port của thiết bị firmware trên Drone (Cấu hình tại ABRConfigs.h)
    NetworkHandler *netHandler = new NetworkHandler(&a);
    QObject::connect(netHandler, &NetworkHandler::onQosDataReceived, abr, &ABRFactory::onSrtCameraConnection);
    QObject::connect(netHandler, &NetworkHandler::onConnectionStateChanged, abr, &ABRFactory::handleSerialStatus);

    netHandler->start(QOS_SERVER_DEFAULT_HOST, QOS_SERVER_DEFAULT_PORT, QOS_SERVER_POLL_INTERVAL_MS);

    return QCoreApplication::exec();
}

