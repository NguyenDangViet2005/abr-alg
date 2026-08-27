#include "SRTAdaptiveBitrateStreaming.h"

SRTAdaptiveBitrateStreaming::SRTAdaptiveBitrateStreaming(QObject *parent)
    : IAdaptiveBitrateStreaming{parent}
{}

void SRTAdaptiveBitrateStreaming::start()
{

}

void SRTAdaptiveBitrateStreaming::stop()
{

}

void SRTAdaptiveBitrateStreaming::reset(int bitrateKbps)
{

}

void SRTAdaptiveBitrateStreaming::setMaxAbrBitrate(unsigned int newMaxAbrBitrate)
{

}

void SRTAdaptiveBitrateStreaming::handleSerialStatus(bool isConnected)
{

}

void SRTAdaptiveBitrateStreaming::handleSetMaxAbrBitrate(int maxBitrate)
{

}

void SRTAdaptiveBitrateStreaming::handleQosCameraConnection(const QVariantList &clients)
{
    for (const QVariant &v : clients) {
        QVariantMap c = v.toMap();
        qDebug() << "[ABR][Camera] peer=" << c["peerAddress"].toString()
                 << "RTT=" << c["msRTT"].toDouble() << "ms"
                 << "Rate=" << c["mbpsSendRate"].toDouble() << "/" << c["mbpsRecvRate"].toDouble() << "Mbps"
                 << "BW=" << c["mbpsBandwidth"].toDouble() << "Mbps"
                 << "Loss=" << c["pktSndLossTotal"].toInt() << "/" << c["pktRcvLossTotal"].toInt();
    }
}

void SRTAdaptiveBitrateStreaming::handleQosControllingConnection(const QVariantList &clients)
{
    for (const QVariant &v : clients) {
        QVariantMap c = v.toMap();
        qDebug() << "[ABR][Ctrl] peer=" << c["peerAddress"].toString()
                 << "RTT=" << c["msRTT"].toDouble() << "ms"
                 << "Rate=" << c["mbpsSendRate"].toDouble() << "/" << c["mbpsRecvRate"].toDouble() << "Mbps"
                 << "BW=" << c["mbpsBandwidth"].toDouble() << "Mbps"
                 << "Loss=" << c["pktSndLossTotal"].toInt() << "/" << c["pktRcvLossTotal"].toInt();
    }
}