#ifndef XBADAPTIVEBITRATESTREAMING_H
#define XBADAPTIVEBITRATESTREAMING_H

#include <QObject>
#include <QString>
#include <QElapsedTimer>
#include "IAdaptiveBitrateStreaming.h"
#include "RouterInterfaceStat.h"
#include "ABRDecision.h"
#include "ABRConfigs.h"

// One ABR instance per interface, fed by the router's per-interface rows
// (UDP 12346, id-60). Distinct from SRTAdaptiveBitrateStreaming, which consumes
// XBSRTFactory's QoS on 12345; the two are deliberately separate engines.
//
// The decision itself lives in the pure decideBitrate() (ABRDecision.h). This
// class only holds state, guards against stale data, resolves the C2 reference
// mode, and emits a proposal. That split is what makes the algorithm testable
// without a socket.
class XBAdaptiveBitrateStreaming : public IAdaptiveBitrateStreaming
{
    Q_OBJECT
public:
    XBAdaptiveBitrateStreaming(const QString &iface, bool isC2Configured,
                               QObject *parent = nullptr);

    void setMaxAbrBitrate(unsigned int newMaxAbrBitrate) override;
    void start() override {}
    void stop()  override {}
    void reset(int bitrateKbps) override;

    QString iface() const { return m_iface; }
    bool    isC2Configured() const { return m_isC2Configured; }
    int     currentBitrate() const { return m_bitrate; }

public slots:
    // One VIDEO or C2 row destined for THIS interface (the factory routes by
    // name). Stores the row and stamps its arrival on the monotonic clock;
    // rows for other interfaces never reach here.
    void handleRouterRow(const RouterInterfaceStat &row);

    // End-of-tick marker (row 602). Runs the stateful wrapper around the core.
    void handleRouterTick(const QString &primaryInterface);

signals:
    // The proposal for this interface. `-1` (NOT_AVAILABLE) is never emitted —
    // the wrapper returns before emitting. `0` means pause.
    void bitrateProposed(const QString &iface, unsigned int kbps);

    // PART B — a reset is warranted. No executor exists (XBQoSService owns no
    // video socket), so this is detect + emit only; see report §6c.
    void requestStreamReset(const QString &iface, const QString &reason);

private:
    int     processQoS();
    C2Mode  c2ModeForThisTick();
    bool    hasFreshVideoRow() const;
    void    updateStuckObservation(double queuingDelayMs, int emittedBitrate);

    const QString m_iface;
    const bool    m_isC2Configured;   // global, from ctor, immutable
    int           m_maxAbr;

    RouterInterfaceStat m_video;
    RouterInterfaceStat m_c2;

    QElapsedTimer m_clock;            // monotonic — never wall-clock (NTP jumps)
    qint64 m_lastVideoMs   = -1;
    qint64 m_lastC2Ms      = -1;
    qint64 m_lastComputeMs = -1;
    qint64 m_lastChangeMs  = 0;

    bool m_c2EverSeen   = false;
    bool m_videoEverSeen = false;
    bool m_staleWarned  = false;
    bool m_c2Warned     = false;

    int  m_bitrate        = ABR_MIN_BITRATE_KBPS;
    int  m_notUsableTicks = 0;
    int  m_lastEmitted    = -1;       // -1 = nothing emitted yet

    // ── Part B: stuck detection ──
    bool   m_wasPrimary  = false;
    bool   m_observing   = false;
    double m_obsQdelay0  = 0.0;
    int    m_obsTicks    = 0;
    qint64 m_lastResetMs = 0;         // 0 = never emitted a reset
};

#endif // XBADAPTIVEBITRATESTREAMING_H
