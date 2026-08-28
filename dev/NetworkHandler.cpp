#include "NetworkHandler.h"

#include <QDebug>

NetworkHandler::NetworkHandler(QObject *parent)
    : QObject{parent}
{}

NetworkHandler::~NetworkHandler()
{
    stop();
}

void NetworkHandler::start()
{
    if (m_udpSocket == nullptr) {
        m_udpSocket = new QUdpSocket(this);
        connect(m_udpSocket, &QUdpSocket::readyRead,
                this, &NetworkHandler::handleReadyRead);
        if (!m_udpSocket->bind(QHostAddress::Any, UDP_LISTEN_PORT)) {
            qDebug() << "[NetworkHandler] bind UDP port" << UDP_LISTEN_PORT
                     << "thất bại:" << m_udpSocket->errorString();
        } else {
            qDebug() << "[NetworkHandler] đang lắng nghe UDP trên port" << UDP_LISTEN_PORT;
        }
    }
}

void NetworkHandler::stop()
{
    if (m_udpSocket) {
        m_udpSocket->close();
        m_udpSocket->deleteLater();
        m_udpSocket = nullptr;
    }
}

void NetworkHandler::handleReadyRead()
{
    while (m_udpSocket && m_udpSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(int(m_udpSocket->pendingDatagramSize()));
        QHostAddress sender;
        quint16 senderPort = 0;
        m_udpSocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
        qDebug() << "[NetworkHandler] nhận datagram" << datagram.size()
                 << "bytes từ" << sender.toString() << ":" << senderPort;
        emit dataReceived(datagram);
    }
}
