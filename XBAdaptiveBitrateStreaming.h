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

    // The camera's report of what it actually achieved for the last request.
    // Closes the loop: the router ABR is otherwise open-loop, so this is the one
    // fact that tells it whether the camera accepted the ask. `achievedKbps <= 0`
    // (camera did not report) is ignored — it must NOT read as "collapsed".
    void handleCameraReportedBitrate(int achievedKbps);

signals:
    // The proposal for this interface. `-1` (NOT_AVAILABLE) is never emitted —
    // the wrapper returns before emitting. `0` means pause.
    void bitrateProposed(const QString &iface, unsigned int kbps);

    // The FULL decision for this tick: the proposal plus the branch/reason that
    // produced it and the C2 mode + ceiling it was made against. The QS debug
    // channel taps this so a sample carries the decision beside its inputs —
    // bitrateProposed alone loses the WHY. Ints, not the enums, so a plain
    // QObject::connect (no qRegisterMetaType) carries it to another object.
    // Emitted every computable tick, even when the bitrate holds.
    void decisionMade(const QString &iface, int kbps, int c2Mode,
                      int branch, int reason, int maxAbr);

    // PART B — a reset is warranted. No executor exists (XBQoSService owns no
    // video socket), so this is detect + emit only; see report §6c.
    void requestStreamReset(const QString &iface, const QString &reason);

private:
    AbrDecision processQoS();
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

    int  m_bitrate        = ABR_INITIAL_BITRATE_KBPS;
    int  m_notUsableTicks = 0;
    // Consecutive FRESH-C2 ticks whose loss is above ABR_C2_LOSS_PAUSE_PCT.
    // Reset by any non-lossy tick, by a non-FRESH C2, and by reset(). Fed to
    // decideBitrate so the pause needs ABR_C2_LOSS_PAUSE_TICKS in a row.
    int  m_c2LossTicks    = 0;
    // Score-history streaks (2026-10-06). Counted ONLY on a fresh-video tick —
    // the freshness guard early-returns before this, so a skipped tick freezes
    // them. A sample below ABR_SCORE_DEC_MIN bumps m_scoreLowStreak and clears
    // the high one (and vice-versa); a deadband sample clears both. BOTH are
    // cleared whenever the bitrate changes, so a slow score backoff needs 3 NEW
    // low samples before it can fire again, and a fast loss-halve cannot be
    // followed by a slow x0.80 on the same event.
    int  m_scoreLowStreak  = 0;
    int  m_scoreHighStreak = 0;
    int  m_lastEmitted    = -1;       // -1 = nothing emitted yet

    // ── Camera closed loop ──
    // Last achieved bitrate the camera reported for a request. 0 = nothing
    // reported yet (or reset), which must NOT read as "collapsed".
    int  m_lastAchievedKbps = 0;
    // achieved/requested below ABR_CAMERA_COLLAPSE_RATIO on the last report.
    // Blocks INCREASE (never a DECREASE). Recomputed on every camera report.
    bool m_cameraCollapsed  = false;
    // C2 mode resolved by the last processQoS(), so handleRouterTick can publish
    // it with the decision without recomputing (which would read a moved clock).
    C2Mode m_lastC2Mode   = C2Mode::NEVER_SEEN;

    // ── Part B: stuck detection ──
    bool   m_wasPrimary  = false;
    bool   m_observing   = false;
    double m_obsQdelay0  = 0.0;
    int    m_obsTicks    = 0;
    qint64 m_lastResetMs = 0;         // 0 = never emitted a reset
};

#endif // XBADAPTIVEBITRATESTREAMING_H
