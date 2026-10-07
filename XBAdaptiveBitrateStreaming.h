#ifndef XBADAPTIVEBITRATESTREAMING_H
#define XBADAPTIVEBITRATESTREAMING_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QElapsedTimer>
#include <QTimer>
#include "IAdaptiveBitrateStreaming.h"
#include "RouterInterfaceStat.h"
#include "ABRConfigs.h"

// One ABR instance per interface, fed by the router's per-interface rows
// (UDP 12346, id-60). Distinct from SRTAdaptiveBitrateStreaming, which consumes
// XBSRTFactory's QoS on 12345; the two are deliberately separate engines.
//
// This is a VERBATIM port of xbfirm's XBAdaptiveBitrateStreaming
// (aiCompressor/XBAdaptiveBitrateStreaming.{h,cpp}) — the standard. The decision
// core — haveLostPackets / rttHigh / checkSenderQueue / setNewBitrate /
// calculateJitter / processTCPInfo / processTelemetryTCPInfo /
// updateStreamingStatus / reset — is copied as-is. Only the SOURCE of the two
// inputs changes:
//
//   VIDEO row (600) -> the tcpInfo slot   (processTCPInfo)
//   C2 row    (601) -> the telemetry slot (processTelemetryTCPInfo)
//
// xbfirm fused two INDEPENDENT measurements (the video socket's tcpi + a
// separate link probe). The router already carries the equivalent pair as the
// video and C2 flows, so mapping C2 -> telemetry restores the second source
// without inventing anything.
//
// Adaptations from xbfirm, each documented at its site:
//   1. Settings/serial/UDP coupling stripped. Thresholds are the xbfirm
//      Settings DEFAULTS (what xbfirm actually runs — see the constants; the
//      header's inline 10.0/15.0 for the two *AFTER_DECREASE values are DEAD,
//      overwritten in xbfirm's ctor from Settings).
//   2. Timing uses the monotonic QElapsedTimer, not wall-clock. xbfirm's
//      QDateTime::currentMSecsSinceEpoch() calls measured ELAPSED ms; the
//      epoch is irrelevant and monotonic is strictly safer (no NTP jumps).
//   3. The two 5s QTimers are xbfirm's transport polling. The router PUSHES at
//      1 Hz, so onNetworkDataTimeout() is repurposed to WARN on router silence
//      rather than request stats over a socket this service does not own.
class XBAdaptiveBitrateStreaming : public IAdaptiveBitrateStreaming
{
    Q_OBJECT
public:
    XBAdaptiveBitrateStreaming(const QString &iface, bool isC2Configured,
                               QObject *parent = nullptr);
    ~XBAdaptiveBitrateStreaming() override;

    void setMaxAbrBitrate(unsigned int newMaxAbrBitrate) override;
    void start() override;
    void stop()  override;
    void reset(int bitrateKbps) override;

    QString iface() const { return m_iface; }
    bool    isC2Configured() const { return m_isC2Configured; }
    int     currentBitrate() const { return static_cast<int>(m_abrState.current_bitrate_kbps); }

public slots:
    // One VIDEO or C2 row destined for THIS interface (the factory routes by
    // name). Splits by connType: VIDEO -> processTCPInfo (the tcpInfo slot),
    // C2 -> processTelemetryTCPInfo (the telemetry slot). xbfirm's two input
    // sources, fed from the router's two flows.
    void handleRouterRow(const RouterInterfaceStat &row);

    // End-of-tick marker (row 602). xbfirm has no equivalent — its algorithm is
    // edge-triggered from processTCPInfo. This exists so the QS debug channel
    // gets a per-tick verdict; it does not feed the algorithm.
    void handleRouterTick(const QString &primaryInterface);

    // The camera's report of what it actually achieved. xbfirm has NO such
    // input; kept only so the existing wiring compiles, and stored for logging.
    // It does NOT gate the algorithm (that would be non-xbfirm logic).
    void handleCameraReportedBitrate(int achievedKbps);

signals:
    // xbfirm's bitrateChanged, renamed. `0` means pause.
    void bitrateProposed(const QString &iface, unsigned int kbps);

    // Per-tick verdict for the QS debug channel. Emitted with the current
    // bitrate; branch/reason are VESTIGIAL (xbfirm has no branch concept) and
    // carry HOLD/NONE. The wire keeps both columns (frozen — the dashboard
    // decodes positionally); only the values are pinned to 0.
    void decisionMade(const QString &iface, int kbps, int c2Mode,
                      int branch, int reason, int maxAbr);

    // PART B — a reset is warranted. No executor exists (XBQoSService owns no
    // video socket), so this is detect + emit only.
    void requestStreamReset(const QString &iface, const QString &reason);

private:
    // ── xbfirm's core ────────────────────────────────────────────────────
    struct tcpInfo {
        quint32 rtt = 0;          // ms
        quint32 min_rtt = 0;      // ms
        double  throughput = 0;   // kbps
        double  loss_rate = 0;    // %
        quint32 siocoutq = 0;     // bytes
        quint32 so_sndbuf = 0;    // bytes
    };
    struct AbrState { unsigned int current_bitrate_kbps = 0; };
    enum LastAction { ABR_NONE, ABR_INCREASE, ABR_DECREASE };

    // xbfirm's UX status, emitted via onStatus (base signal).
    enum StreamingQualityStatus {
        PLAYING_AUTO = 0, OPTIMIZING = 1, POOR_NETWORK = 2, BUFFERING = 3, LOADING = 4
    };

    void processTCPInfo(const tcpInfo &tcpi);

    // xbfirm's 7-arg overload: the unit-conversion + _cameraStatus gate stage.
    // Inputs are RAW: rtt/min_rtt in MICROseconds, throughput in BYTES/s,
    // loss_rate as a lost-packet COUNT (the body divides it by delta(tcpiSegsout)
    // to get a percent), siocoutq/so_sndbuf bytes, tcpiSegsout a packet count.
    // These are exactly the router's raw counter block, so the port feeds them
    // with no unit fudge.
    void processTCPInfo(quint32 rtt, quint32 min_rtt, double throughput,
                        double loss_rate, quint32 siocoutq, quint32 so_sndbuf,
                        int tcpiSegsout);

    // xbfirm's 11-arg overload: the wire-shaped entry, forwarding to the 7-arg
    // overload above. DROPS tcpiSndCwnd/tcpiSndMss/tcpiLastAckRecv — exactly as
    // xbfirm does, so the port keeps the same (smaller) live field set. rtt and
    // tcpiMinRtt are MICROseconds.
    void processTCPInfo(double rtt, double deliveryRate, double tcpiRtt,
                        int tcpiLoss, int tcpiSegsout, int tcpiSndCwnd,
                        int tcpiSndMss, int tcpiLastAckRecv, int sockOutq,
                        int sockSndBuf, int tcpiMinRtt);

    // xbfirm's telemetry slot, fed from the C2 (601) row. Same raw units as the
    // 11-arg overload: rtt/tcpiMinRtt MICROseconds, tcpiLoss/tcpiSegsout packet
    // counts. The body derives the telemetry loss percent from
    // delta(tcpiLoss)/delta(tcpiSegsout) itself, so no synthetic counter is
    // needed — the router sends the real counters.
    void processTelemetryTCPInfo(double rtt, double deliveryRate, double tcpiRtt,
                                 int tcpiLoss, int tcpiSegsout, int tcpiSndCwnd,
                                 int tcpiSndMss, int tcpiLastAckRecv, int sockOutq,
                                 int sockSndBuf, int tcpiMinRtt);

    bool haveLostPackets(const tcpInfo &tcpi);
    bool rttHigh(const tcpInfo &tcpi);
    double calculateJitter();
    void checkSenderQueue(const tcpInfo &tcpi);
    void setNewBitrate(unsigned int new_bitrate_kbps);
    void updateStreamingStatus(const tcpInfo &tcpi, bool bitrateChanged);

    // Phase 01 (PORT DEVIATION, not xbfirm): true when the current video-loss
    // break started right after OUR own step-up, i.e. the loss is our
    // overshoot rather than a weak link. Causal/timing test, never an absolute
    // score comparison — see the definition for why.
    bool isOvershoot() const;

    void onNetworkDataTimeout();
    void onStatusBroadcastTimeout();

    // ── Bitrate constraints (xbfirm) ─────────────────────────────────────
    static constexpr unsigned int MIN_BITRATE_KBPS  = 30;
    static constexpr unsigned int MAX_BITRATE_KBPS  = 2000;
    static constexpr unsigned int BITRATE_STEP_KBPS = 100;
    static constexpr unsigned int MIN_BITRATE_CHANGE_THRESHOLD = 30;

    // ── Algorithm parameters (xbfirm; *AFTER_DECREASE = Settings defaults) ──
    static constexpr int     RTT_HISTORY_SIZE = 10;
    static constexpr quint32 RTT_HIGH_THRESHOLD_MS = 500;   // rttHigh hardcodes 500
    static constexpr double  QUEUE_DELAY_HIGH_THRESHOLD_MS = 400.0;
    static constexpr double  QUEUE_DELAY_LOW_THRESHOLD_MS  = 100.0;
    static constexpr double  QUEUE_DELAY_LOW_AFTER_DECREASE_MS = 20.0;
    static constexpr double  RTT_STABLE_THRESHOLD_MS = 50.0;
    static constexpr double  RTT_STABLE_AFTER_DECREASE_MS = 20.0;
    static constexpr double  JITTER_STABLE_THRESHOLD_MS = 15.0;
    static constexpr double  TELEMETRY_STABLE_TIMEOUT_MS = 3000.0;
    static constexpr unsigned int TELEMETRY_KICK_BITRATE_KBPS = 500;
    static constexpr qint64  BITRATE_INCREASE_COOLDOWN_MS = 2000;

    // ── Phase 01: overshoot classifier + ceiling (PORT DEVIATION) ─────────
    // A break counts as "our overshoot" only if it lands within this many
    // ticks after our own step-up. Too small -> overshoot misread as weak
    // (harsh MIN); too large -> weak misread as overshoot (stuck high).
    // Tune against the field log (Phase 04).
    static constexpr int OVERSHOOT_WINDOW_TICKS = 3;
    // The same window as wall-clock, so a data gap cannot freeze the tick
    // counter and make an old step-up look recent. ~3 ticks at the router's
    // 1 Hz push.
    static constexpr qint64 OVERSHOOT_WINDOW_MS = 3000;
    // The score at step-up must have been at least this good for the break to
    // be "our overshoot". A margin, not an absolute grade — it only separates
    // "we pushed from a healthy link" from "we pushed from an already-poor one".
    static constexpr double OVERSHOOT_MIN_SCORE_AT_STEPUP = 70.0;
    // The wire trend at step-up must NOT already be falling, or the link was
    // degrading BEFORE we pushed — that is a weak link, not our overshoot. Trend
    // ordinals match ScoringModule: 0=RAPID_INCREASE..2=STABLE, 3=DECREASE,
    // 4=RAPID_DECREASE. This is the "score already falling before T" half of the
    // brainstorm's discriminator, implemented without a score history.
    static constexpr int OVERSHOOT_MAX_TREND_AT_STEPUP = 2;   // <= STABLE

    // ── Core state (xbfirm) ──────────────────────────────────────────────
    AbrState m_abrState;
    QVector<quint32> m_rttHistory;
    LastAction m_lastAbrAction = ABR_NONE;
    unsigned int m_problematicBitrateThreshold = MAX_BITRATE_KBPS;
    qint64 m_lastQualityChangeTime = 0;
    double m_avgSiocoutq = 0.0;
    unsigned int m_maxAbr;

    // ── Phase 01: ceiling + causal overshoot state (PORT DEVIATION) ───────
    // m_ceilingKbps: the STEADY-STATE ramp never exceeds this. Found once on
    // overshoot, then respected (Phase 03's NetworkReference is the only thing
    // that lifts it). One documented exception: the telemetry ABR kick in
    // processTelemetryTCPInfo may exceed it (a post-outage probe, see there).
    // m_lastGoodKbps: the bitrate immediately BEFORE the break-causing step-up
    // — the value we fall back to. m_scoreAtStepUp / m_ticksSinceStepUp /
    // m_lastStepUpMs are the causal triple the classifier reads.
    unsigned int m_ceilingKbps = MAX_BITRATE_KBPS;
    unsigned int m_lastGoodKbps = 0;
    double m_scoreAtStepUp = 0.0;
    int m_trendAtStepUp = 0;
    int m_ticksSinceStepUp = 0;
    qint64 m_lastStepUpMs = 0;

    // Advisor coefficients. xbfirm's CellularPredictive drove these; with no
    // predictor they stay at xbfirm's ctor defaults (1.0), so gainDown=1.0
    // makes the qdelay>400 decrease a no-op and gainUp=1.0 gives the plain
    // +BITRATE_STEP_KBPS step. Kept as members for parity.
    double m_alpha = 1.0;
    double m_gainUp = 1.0;
    double m_gainDown = 1.0;

    // UX status (xbfirm updateStreamingStatus).
    StreamingQualityStatus m_currentStatus = LOADING;
    StreamingQualityStatus m_previousStatus = LOADING;
    int m_consecutivePoorConditions = 0;
    qint64 m_lastStatusChangeTime = 0;

    bool m_isStarted = false;

    // Cumulative tracking for the delta calculation (xbfirm).
    int _lastTcpiSegsout = 0;
    int _lastTelemetrySegsout = 0;

    // Telemetry (C2) data (xbfirm).
    quint32 m_telemetryRtt = 0;
    quint32 m_telemetryMinRtt = 0;
    double  m_telemetryLossRate = 0.0;
    qint64  m_telemetryStableSince = 0;

    // Gate on the telemetry slot (xbfirm's _haveTelemetry). Set true once a C2
    // row has been seen — "the C2 subflow exists". If C2 is never present,
    // rttHigh never fires and the telemetry-loss branch reads 0, mirroring
    // xbfirm with no telemetry source.
    bool _haveTelemetry = false;
    // xbfirm's _cameraStatus: false pins the video path off while telemetry loss
    // is severe. Set false by processTelemetryTCPInfo on >1% loss; restored
    // after TELEMETRY_STABLE_TIMEOUT_MS of clean telemetry.
    bool _cameraStatus = true;

    // ── Router wiring (not xbfirm) ───────────────────────────────────────
    const QString m_iface;
    const bool m_isC2Configured;

    QElapsedTimer m_clock;

    RouterInterfaceStat m_video;   // last VIDEO row
    RouterInterfaceStat m_c2;      // last C2 row
    qint64 m_lastNetworkDataTime = 0;
    qint64 m_lastResetMs = 0;      // 0 = never emitted a reset (Part B rate-limit)

    // Camera closed loop — stored for logging only (see handleCameraReportedBitrate).
    int m_lastAchievedKbps = 0;

    QTimer *m_networkDataTimer = nullptr;
    QTimer *m_statusBroadcastTimer = nullptr;
};

#endif // XBADAPTIVEBITRATESTREAMING_H
