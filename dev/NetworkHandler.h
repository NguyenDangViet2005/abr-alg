#ifndef NETWORKHANDLER_H
#define NETWORKHANDLER_H

#include <QObject>
#include <QUdpSocket>
#include <QByteArray>

class NetworkHandler : public QObject
{
    Q_OBJECT
public:
    explicit NetworkHandler(QObject *parent = nullptr);
    ~NetworkHandler();

    // Port UDP lắng nghe (trùng với SRT QoS listener bên ngoài).
    static constexpr quint16 UDP_LISTEN_PORT = 12345;

    void start();
    void stop();

signals:
    // Phát mỗi khi nhận được 1 datagram UDP từ port 12345.
    void dataReceived(const QByteArray &datagram);

private slots:
    void handleReadyRead();

private:
    QUdpSocket* m_udpSocket = nullptr;
};

#endif // NETWORKHANDLER_H
