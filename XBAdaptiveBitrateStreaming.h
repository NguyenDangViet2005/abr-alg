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
// updateStreamingStatus / reset — is copied as-is, EXCEPT the deviations marked
// "PORT DEVIATION" at their sites (Phase 01/06: overshoot classifier + ceiling,
// ×0.80 gentle step, rttHigh re-pointed at the video row, qdelay decrease no
// longer a no-op). Only the SOURCE of the two inputs changes:
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

    // xbfirm handleSerialStatus, verbatim. The telemetry SOURCE's connection
    // state drives _haveTelemetry. The router port has no serial link, so this
    // is fed from the C2 flow's presence (see handleRouterRow) — or left at its
    // default `false` when no telemetry source is wired. It is deliberately NOT
    // a per-row "C2 arrived" toggle: xbfirm gates the slot on the SOURCE's
    // connection, not on a sample.
    void handleSerialStatus(bool isConnected);

signals:
    // xbfirm's bitrateChanged, renamed. `0` means pause.
    void bitrateProposed(const QString &iface, unsigned int kbps);

    // Per-tick verdict for the QS debug channel. Emitted with the current
    // bitrate; branch/reason are VESTIGIAL (xbfirm has no branch concept) and
    // carry HOLD/NONE. The wire keeps both columns (frozen — the dashboard
    // decodes positionally); only the values are pinned to 0.
    //
    // `ceiling` is the learned ceiling for the QS SAMPLE's ceilingKbps field:
    // 0 while none has been found (the ramp is still free to m_maxAbr), else the
    // value the ramp is held to. 0 is the wire's "not yet found" sentinel — it
    // is never a literal 0 kbps ceiling (a real ceiling is always >= MIN+STEP).
    void decisionMade(const QString &iface, int kbps, int c2Mode,
                      int branch, int reason, int maxAbr, int ceiling);

    // The instant a new ceiling is learned (Phase 01 overshoot). Distinct from
    // decisionMade, which is per-tick: this is edge-triggered at the moment
    // m_ceilingKbps changes, so the QS channel can fire QS_EVT_ABR_CEILING
    // WITHOUT waiting for the next 1 Hz SAMPLE to carry the field.
    void ceilingChanged(const QString &iface, unsigned int kbps);

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

    // Sets m_ceilingKbps and emits ceilingChanged ONLY when the value actually
    // moves. The single place the ceiling changes, so the edge-triggered event
    // cannot be missed or double-fired (a per-tick emit would spam the channel
    // once a second for an unchanged ceiling).
    void setCeiling(unsigned int kbps);

    // Phase 01/06: the overshoot reaction — fall back to last-good and cap the
    // ramp there. Shared by haveLostPackets (loss break) and rttHigh (bufferbloat
    // break) so the two entry points cannot drift. Returns the target bitrate for
    // the caller's log (m_ceilingKbps is updated by the time it returns).
    unsigned int applyOvershootCeiling();

    // Phase 07: the ONLY gate that lets the ramp cross m_ceilingKbps. Returns
    // false by default (no NetworkReference source wired yet), so a learned
    // ceiling is held for the whole session. Phase 03 overrides it from the
    // signal-strength / GPS reference: when the environment is proven to have
    // changed it returns true, the ceiling is LIFTED (re-probe) and a later
    // overshoot learns a new one.
    bool allowExceedCeiling() const;

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
    // Phase 06: raised 500 -> 700. rttHigh now reads the VIDEO rtt-min_rtt delta
    // (a region-independent bufferbloat gauge); a higher bar tolerates a
    // high-latency baseline and only brakes on a genuinely large queue.
    static constexpr quint32 RTT_HIGH_THRESHOLD_MS = 700;
    // Phase 06: the ONE gentle-decrease factor, shared by the three decrease
    // paths (weak loss, rttHigh bufferbloat, qdelay congestion) so they cannot
    // drift. 0.80 is xbfirm's own rttHigh factor, not an invention.
    static constexpr double  ABR_DECREASE_FACTOR = 0.80;
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
    // The classifier is EVENT-causal, NOT time-windowed: a break counts as
    // "our overshoot" when the most recent bitrate action was an INCREASE (we
    // raised and nothing has lowered since) AND that raise started from a
    // healthy link. There is deliberately NO clock/tick window — the field log
    // proved the break can land ~26 s after the step-up, which no short window
    // covers, and a wide one would call every break an overshoot.
    //
    // The score at step-up must have been at least this good: a margin, not an
    // absolute grade — it separates "we pushed from a healthy link" from "we
    // pushed from an already-poor one".
    static constexpr double OVERSHOOT_MIN_SCORE_AT_STEPUP = 70.0;
    // The wire trend at step-up must NOT already be falling, or the link was
    // degrading BEFORE we pushed — that is a weak link, not our overshoot. Trend
    // ordinals match ScoringModule: 0=RAPID_INCREASE..2=STABLE, 3=DECREASE,
    // 4=RAPID_DECREASE.
    static constexpr int OVERSHOOT_MAX_TREND_AT_STEPUP = 2;   // <= STABLE

    // ── Phase 07: behavior hardening (PORT DEVIATION) ─────────────────────
    // After an overshoot, HOLD for this long and ACCEPT the loss: the loss that
    // trails a step-up is the queue draining, not a new break. Without it the
    // next tick re-decreases and the ceiling ends up one step short of capacity.
    static constexpr qint64  OVERSHOOT_SETTLE_MS = 3000;
    // Telemetry (C2) loss percent above which the LINK itself is called weak
    // (xbfirm 0.3 -> 0.4: the router's C2 flow is noisier than xbfirm's probe).
    static constexpr double  TELEMETRY_LOSSY_THRESHOLD_PCT = 0.4;
    // A telemetry-lossy tick only means the link is weak after this many
    // CONSECUTIVE lossy C2 samples — a 1-tick C2 spike must not drop the video
    // straight to MIN (G3). Mirrors ZERO_THROUGHPUT_CONFIRM_TICKS.
    static constexpr int     TELEMETRY_LOSSY_CONFIRM_TICKS = 2;
    // throughput==0 is a real collapse only after this many CONSECUTIVE zero
    // ticks — a 1-tick spike must not drop straight to MIN.
    static constexpr int     ZERO_THROUGHPUT_CONFIRM_TICKS = 2;

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
    // — the value we fall back to. m_scoreAtStepUp / m_trendAtStepUp are the
    // causal pair isOvershoot() reads.
    unsigned int m_ceilingKbps = MAX_BITRATE_KBPS;
    unsigned int m_lastGoodKbps = 0;
    double m_scoreAtStepUp = 0.0;
    int m_trendAtStepUp = 0;

    // ── Phase 07: settle window + zero-throughput debounce (PORT DEVIATION) ─
    // m_settleUntil: monotonic ms until which the algorithm HOLDs and accepts
    // loss after an overshoot (0 = inactive). m_zeroThroughputTicks: consecutive
    // throughput==0 ticks (must reach ZERO_THROUGHPUT_CONFIRM_TICKS to act).
    // m_telemetryLossyTicks: consecutive telemetry-lossy ticks (must reach
    // TELEMETRY_LOSSY_CONFIRM_TICKS before the weak-link→MIN branch acts).
    qint64 m_settleUntil = 0;
    int    m_zeroThroughputTicks = 0;
    int    m_telemetryLossyTicks = 0;

    // Advisor coefficients. xbfirm's CellularPredictive drove these; with no
    // predictor they stay at xbfirm's ctor defaults (1.0). gainUp=1.0 gives the
    // plain +BITRATE_STEP_KBPS step. m_gainDown is now UNUSED by the qdelay path
    // (Phase 06 replaced it with ABR_DECREASE_FACTOR, which is always applied);
    // kept as a member for parity so an advisor can be re-wired later.
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

    // Gate on the telemetry slot (xbfirm's _haveTelemetry). xbfirm sets it from
    // the telemetry SOURCE's connection state via handleSerialStatus; here a C2
    // row stands in for that (see handleRouterRow). Defaults to `false`: with no
    // telemetry source wired, the telemetry-loss branch reads 0 (dead) and the
    // telemetry-derived inputs (m_telemetryRtt/minRtt, m_telemetryLossRate) stay
    // 0. rttHigh no longer depends on this gate — Phase 06 re-pointed it at the
    // VIDEO row — so bufferbloat detection works without a C2 source.
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
