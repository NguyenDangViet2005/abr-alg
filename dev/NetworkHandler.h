#ifndef NETWORKHANDLER_H
#define NETWORKHANDLER_H

#include <QObject>

class NetworkHandler : public QObject
{
    Q_OBJECT
public:
    explicit NetworkHandler(QObject *parent = nullptr);

signals:
};

#endif // NETWORKHANDLER_H
