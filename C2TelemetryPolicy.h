#ifndef C2TELEMETRYPOLICY_H
#define C2TELEMETRYPOLICY_H

#include "C2TelemetryStat.h"
#include <QVector>
#include <limits>

// C2 can constrain the camera/RF decision, never supply camera bandwidth.
class C2TelemetryPolicy {
public:
    enum Level { Normal = 0, High = 1, Critical = 2 };
    struct Decision {
        Level level = Normal;
        bool signal = false;
        bool inputsReady = false;
        bool holdIncrease = true;
        unsigned int ceilingKbps = std::numeric_limits<unsigned int>::max();
        double worstRttMs = -1;
    };
    void reset();
    void update(const QVector<C2TelemetryStat> &samples, qint64 nowMs);
    Decision evaluate(qint64 nowMs);
    unsigned int limit(unsigned int requestedKbps, unsigned int currentKbps, qint64 nowMs);
private:
    QVector<C2TelemetryStat> m_samples;
    qint64 m_received = -1, m_recoverySince = -1;
    Level m_level = Normal;
    Level m_recoveryLevel = Normal;
    bool m_wasReady = false;
};

#endif
