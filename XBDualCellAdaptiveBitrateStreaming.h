#ifndef XBDUALCELLADAPTIVEBITRATESTREAMING_H
#define XBDUALCELLADAPTIVEBITRATESTREAMING_H

#include <QObject>
#include "XBAdaptiveBitrateStreaming.h"

class XBDualCellAdaptiveBitrateStreaming : public XBAdaptiveBitrateStreaming
{
    Q_OBJECT
public:
    explicit XBDualCellAdaptiveBitrateStreaming(QObject *parent = nullptr);;
signals:
};

#endif // XBDUALCELLADAPTIVEBITRATESTREAMING_H
