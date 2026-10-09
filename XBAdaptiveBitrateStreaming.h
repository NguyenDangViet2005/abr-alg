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
#include "ABRTuning.h"       // mọi tham số ABR (ABR_*) — xem ABRTuning.h
#include "NetworkReference.h"

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

    // Phase 09 (headroom): set the steady-state cap fraction (0.5..1.0). Called
    // once at creation from ABRFactory (from --abr-headroom / the #define). Not
    // live-tunable: maxAbr is also create-time, and a mid-session change would
    // move the cap under a ramp already parked. Out-of-range is clamped + logged
    // so a CLI typo is visible rather than silently snapping to the floor.
    void setHeadroomFactor(double factor);

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

    // Radio strength for THIS interface (packet 603), routed by name from the
    // factory. Pushed straight into the NetworkReference; the ref owns all the
    // interpretation (baseline, staleness, debounce).
    void handleNetworkStrength(int strength);

    // xbfirm handleSerialStatus, verbatim. The telemetry SOURCE's connection
    // state drives _haveTelemetry. The router port has no serial link, so the
    // ctor seeds _haveTelemetry from m_isC2Configured (a DEPLOYMENT fact set in
    // main.cpp, not a per-packet one) — see the ctor. This setter is kept for a
    // future real serial link but is NOT called per C2 row any more (Phase 11).
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
    // `cap` (Phase 09) is the steady-state ramp cap = factor × min(ceiling,
    // maxAbr); always a real value (no sentinel).
    void decisionMade(const QString &iface, int kbps, int c2Mode,
                      int branch, int reason, int maxAbr, int ceiling, int cap);

    // The instant a new ceiling is learned (Phase 01 overshoot). Distinct from
    // decisionMade, which is per-tick: this is edge-triggered at the moment
    // m_ceilingKbps changes, so the QS channel can fire QS_EVT_ABR_CEILING
    // WITHOUT waiting for the next 1 Hz SAMPLE to carry the field.
    void ceilingChanged(const QString &iface, unsigned int kbps);

    // PART B — a reset is warranted. The executor lives in XBQoSService (this
    // class owns no video socket): the factory relays this signal and the
    // service maps it to CameraControl::requestStreamRefresh. Emitted from the
    // ladder's >70% band, rate-limited at the emit site (m_lastResetMs).
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
    // loss_rate as a lost-packet COUNT (the body divides it by a 5-tick
    // Σ delta(tcpiSegsout) window to get a percent — Phase 08 (A)), siocoutq/
    // so_sndbuf bytes, tcpiSegsout a packet count. These are exactly the router's
    // raw counter block, so the port feeds them with no unit fudge.
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
    // NetworkReference's strength baseline: when the current strength is HIGHER
    // than the strength recorded at ceiling-set time (and held past the
    // debounce), the environment is proven better → true, the ceiling is LIFTED
    // (re-probe) and a later overshoot learns a new one. GPS is a later extra
    // gate. Absence of info (stale/9/no baseline) is false.
    bool allowExceedCeiling() const;

    // Phase 08 (C): the ceiling-escape FALLBACK. True only when there is NO
    // USABLE strength source (m_networkRef.hasUsableStrength() false) AND we have
    // held at the ceiling for the current backoff. Composed into
    // allowExceedCeiling().
    bool ceilingReprobeDue() const;

    // Phase 09 (headroom, PORT DEVIATION): the STEADY-STATE ramp cap. The video
    // ramp holds at `effectiveHeadroomFactor() × min(ceiling, maxAbr)` instead of
    // at the ceiling itself, reserving the remainder of the link for other tasks
    // (C2/SRT, control traffic, bursts). Derived, NOT stored: m_ceilingKbps stays
    // the TRUE ceiling (the wire/dashboard report it unchanged), so headroom is a
    // separate policy layered on top. Consumed by the ramp gate AND the decision
    // report. Applies even at m_maxAbr (no learned ceiling) — the user wants the
    // reservation on a healthy link too.
    unsigned int steadyCapKbps() const;

    // Phase 09b (2-tier headroom, PORT DEVIATION): pick the headroom factor for
    // the CURRENT ceiling — a WEAK link reserves more. Thresholds/values live in
    // ABRTuning.h (ABR_LOW/CRITICAL_CEILING_*). Keyed on min(m_ceilingKbps,
    // m_maxAbr), the same base steadyCapKbps() multiplies — never on the cap
    // itself (cap depends on this factor → circular). Below 500 -> 0.50; below
    // 1000 -> 0.60; else m_headroomFactor (0.80, or --abr-headroom). The tier is
    // then min()-clamped by m_headroomFactor so it can only be STRICTER, never
    // looser, than the operator's --abr-headroom.
    double effectiveHeadroomFactor() const;

    // Phase 01 (PORT DEVIATION, not xbfirm): true when the current video-loss
    // break started right after OUR own step-up, i.e. the loss is our
    // overshoot rather than a weak link. Causal/timing test, never an absolute
    // score comparison — see the definition for why.
    bool isOvershoot() const;

    void onNetworkDataTimeout();
    void onStatusBroadcastTimeout();

    // ── Tuning constants: see ABRTuning.h ────────────────────────────────
    // Every numeric tunable (bitrate limits, thresholds, factors, tick counts)
    // now lives in ABRTuning.h as an ABR_-prefixed #define — one place to edit,
    // grouped most-edited-first, with Vietnamese comments. The class no longer
    // declares them as members: a #define of the same name would rewrite the
    // declaration itself. The prose below keeps the REASONING (why each value
    // is what it is); the value itself is in ABRTuning.h.
    //
    // ABR_RTT_HIGH_THRESHOLD_MS (500, xbfirm): rttHigh reads the VIDEO rtt−min_rtt
    // delta (a region-independent bufferbloat gauge) — 500ms of STANDING queue is
    // heavy lag, and the delta already discounts baseline latency, so there is
    // nothing to "tolerate" by raising it. Phase 06 briefly raised it to 700;
    // reverted (let video lag ~1s before braking, out of tune with the 400ms
    // drain-time gate in checkSenderQueue). A false decrease is cheap (×0.80 +
    // settle); a miss is dear.
    //
    // ABR_DECREASE_FACTOR (0.80, xbfirm): the ONE gentle-decrease factor, shared
    // by the three decrease paths (weak loss, rttHigh bufferbloat, qdelay
    // congestion) so they cannot drift.
    //
    // ── Phase 01: overshoot classifier + ceiling (PORT DEVIATION) ─────────
    // The classifier is EVENT-causal, NOT time-windowed: a break counts as
    // "our overshoot" when the most recent bitrate action was an INCREASE (we
    // raised and nothing has lowered since) AND that raise started from a
    // healthy link. There is deliberately NO clock/tick window — the field log
    // proved the break can land ~26 s after the step-up, which no short window
    // covers, and a wide one would call every break an overshoot.
    //
    // ABR_OVERSHOOT_MIN_SCORE_AT_STEPUP (70): the score at step-up must have been
    // at least this good — a margin, not an absolute grade; it separates "we
    // pushed from a healthy link" from "we pushed from an already-poor one".
    // ABR_OVERSHOOT_MAX_TREND_AT_STEPUP (2 = STABLE): the wire trend at step-up
    // must NOT already be falling, or the link was degrading BEFORE we pushed —
    // that is a weak link, not our overshoot. Ordinals match ScoringModule:
    // 0=RAPID_INCREASE..2=STABLE, 3=DECREASE, 4=RAPID_DECREASE.
    //
    // Phase 08 (B): ABR_OVERSHOOT_CONFIRM_TICKS (2) — a break must repeat this
    // many CONSECUTIVE ticks before the CEILING latches. The decrease is
    // immediate and recoverable; the ceiling is permanent (it holds the ramp for
    // the session), so it waits for confirmation. The field log's 5% spike was a
    // one-tick artifact that pinned the ramp at 431 kbps forever; a clean tick in
    // between discards the candidate.
    //
    // Phase 08 (C): ABR_CEILING_REPROBE_BASE_MS (30000) / _MAX_MS (120000) — with
    // NO strength source, environmentChanged() is permanently false, so a learned
    // ceiling would never lift (permanent underuse). After BASE_MS holding at the
    // ceiling the ABR takes one probing step; if that re-overshoots, the ceiling
    // re-latches and the wait DOUBLES, capped at MAX_MS. The fallback is disabled
    // once any 603 arrives — the primary environmentChanged() path owns the escape.
    //
    // ── Phase 07: behavior hardening (PORT DEVIATION) ─────────────────────
    // ABR_OVERSHOOT_SETTLE_MS (3000): after an overshoot, HOLD and ACCEPT the
    // loss — the loss trailing a step-up is the queue draining, not a new break.
    // Without it the next tick re-decreases and the ceiling ends up one step short.
    // ABR_ZERO_THROUGHPUT_CONFIRM_TICKS (2): a 1-tick zero-throughput must not
    // drop the video straight to MIN — it needs this many CONSECUTIVE ticks.
    // (Phase 11 removed the telemetry-lossy branch entirely — C2 no longer
    // triggers a weak-link drop; the ladder keys on video loss only.)
    // ABR_LOSS_WINDOW_TICKS (5, Phase 08 A): the VIDEO loss percent is a
    // Σlost/ΣsegsOut over this many ticks. The video flow sends only ~70-80 segs
    // per tick, so a single lost packet reads as ~1.3% — over the 0.8% trigger.
    // At 5×70=350 segments one packet is 0.29%. VIDEO only — the C2 slot's ~550
    // segs/tick denominator is already adequate.

    // ── Core state (xbfirm) ──────────────────────────────────────────────
    AbrState m_abrState;
    QVector<quint32> m_rttHistory;
    LastAction m_lastAbrAction = ABR_NONE;
    unsigned int m_problematicBitrateThreshold = ABR_MAX_BITRATE_KBPS;
    qint64 m_lastQualityChangeTime = 0;
    double m_avgSiocoutq = 0.0;
    unsigned int m_maxAbr;

    // ── Phase 01: ceiling + causal overshoot state (PORT DEVIATION) ───────
    // m_ceilingKbps: the TRUE learned link ceiling. Found once on overshoot,
    // then respected (Phase 03's NetworkReference is the only thing that lifts
    // it). The STEADY-STATE ramp is NOT held here directly any more — Phase 09
    // holds it at steadyCapKbps() = effectiveHeadroomFactor() × min(this, m_maxAbr),
    // a headroom-reserving cap BELOW this. The wire/dashboard still report THIS
    // (the real ceiling). One documented exception: the telemetry ABR kick in
    // processTelemetryTCPInfo may exceed it (a post-outage probe, see there).
    // m_lastGoodKbps: the bitrate immediately BEFORE the break-causing step-up
    // — the value we fall back to. m_scoreAtStepUp / m_trendAtStepUp are the
    // causal pair isOvershoot() reads.
    unsigned int m_ceilingKbps = ABR_MAX_BITRATE_KBPS;

    // Phase 09 (headroom, PORT DEVIATION): the fraction of the ceiling the
    // steady-state ramp is allowed to reach. 0.80 = reserve 20% of the link for
    // other tasks. SEPARATE from ABR_DECREASE_FACTOR (also 0.80 today) — that is
    // the break step-DOWN multiplier; this is the steady-state UP cap. Same
    // value, different meaning: never alias them. Configurable at runtime via
    // setHeadroomFactor() (--abr-headroom). Clamped to [0.5, 1.0]. This is the
    // DEFAULT (healthy-link) tier only — a low ceiling overrides it via
    // effectiveHeadroomFactor() (Phase 09b, see ABR_LOW/CRITICAL_CEILING_*).
    double m_headroomFactor = 0.80;
    unsigned int m_lastGoodKbps = 0;
    double m_scoreAtStepUp = 0.0;
    int m_trendAtStepUp = 0;

    // Phase 08 (B): the overshoot EPISODE. m_pendingCeiling != 0 means a break
    // tick has been seen and the ceiling is staged but not yet latched;
    // isOvershoot() returns true for as long as the episode is open so the NEXT
    // break tick re-enters the same branch (without this the H1 fix — which sets
    // ABR_DECREASE after the first break — would make a 2nd consecutive break
    // impossible to classify). m_overshootConfirmTicks counts the break ticks in
    // the episode. A tick that is neither a loss nor a bufferbloat break closes
    // the episode (processTCPInfo(const tcpInfo&)).
    unsigned int m_pendingCeiling = 0;
    int m_overshootConfirmTicks = 0;

    // The radio-strength reference that is the ONLY thing allowed to lift the
    // learned ceiling (allowExceedCeiling → environmentChanged). Held BY VALUE:
    // m_iface is const per instance, so the one-ref-per-interface mapping is
    // automatic and its lifetime rides the instance (no pointer to manage).
    NetworkReference m_networkRef;

    // ── Phase 08 (C): ceiling-escape fallback state (PORT DEVIATION) ───────
    // Used only while there is NO USABLE strength source
    // (m_networkRef.hasUsableStrength() false).
    // m_ceilingHoldSince: ms when the hold at the ceiling began (0 = not holding).
    // m_ceilingReprobeBackoffMs: the current wait (base 30s, doubles on re-latch).
    // m_reprobeInFlight: a FALLBACK lift happened; a later latch doubles the wait.
    qint64 m_ceilingHoldSince = 0;
    qint64 m_ceilingReprobeBackoffMs = ABR_CEILING_REPROBE_BASE_MS;
    bool   m_reprobeInFlight = false;

    // ── Phase 07: settle window + zero-throughput debounce (PORT DEVIATION) ─
    // m_settleUntil: monotonic ms until which the algorithm HOLDs and accepts
    // loss after an overshoot OR after a ladder decrease (0 = inactive).
    // m_zeroThroughputTicks: consecutive throughput==0 ticks (must reach
    // ABR_ZERO_THROUGHPUT_CONFIRM_TICKS to act).
    qint64 m_settleUntil = 0;
    int    m_zeroThroughputTicks = 0;
    // Phase 10 (recovery-hold, PORT DEVIATION): consecutive CLEAN ticks while
    // PINNED at MIN (current <= MIN). The ramp only resumes once this reaches
    // ABR_RECOVERY_CONFIRM_TICKS, so a lone clean tick after a drop to MIN cannot
    // ramp straight back into a link whose buffer has not drained ("đơ đơ").
    // Reset to 0 by any break tick (processTCPInfo), by any non-clean ramp tick
    // (checkSenderQueue end-reset), and by the two discontinuity paths that bypass
    // both — reset() (M1) and the telemetry-collapse branch (M2). Inert above MIN
    // — the normal 1-tick ramp is unchanged there.
    int    m_cleanRecoveryTicks = 0;

    // Advisor coefficients. xbfirm's CellularPredictive drove these; with no
    // predictor they stay at xbfirm's ctor defaults (1.0). gainUp=1.0 gives the
    // plain +ABR_BITRATE_STEP_KBPS step. m_gainDown is now UNUSED by the qdelay path
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

    // Phase 08 (A): the VIDEO loss-percent sliding window — parallel vectors of
    // the last ABR_LOSS_WINDOW_TICKS ticks' (lost, deltaSegsOut). Rebuilt from the
    // router's raw counters (lost is a per-tick count, deltaSegsOut the delta of
    // the cumulative segsOut), so the percent is the TRUE windowed loss rate.
    QVector<int> m_lossWindowLost;
    QVector<int> m_lossWindowSegs;
    // Phase 08 (A): did THIS tick lose at least one packet? The windowed rate
    // above stays elevated for ABR_LOSS_WINDOW_TICKS after a single burst, so without
    // this a burst would re-fire haveLostPackets on every one of those ticks —
    // five ×0.80 self-congestion decreases from ONE event, and B's "2 consecutive
    // breaks" would be satisfied by the same burst. A break requires FRESH loss;
    // the elevated-but-stale window is memory, not a new break.
    bool m_videoLossThisTick = false;

    // Telemetry (C2) data (xbfirm).
    quint32 m_telemetryRtt = 0;
    quint32 m_telemetryMinRtt = 0;
    double  m_telemetryLossRate = 0.0;
    qint64  m_telemetryStableSince = 0;

    // Gate on the telemetry slot (xbfirm's _haveTelemetry). xbfirm sets it from
    // the telemetry SOURCE's connection state via handleSerialStatus; here it is
    // seeded in the ctor from m_isC2Configured (a DEPLOYMENT fact from main.cpp),
    // NOT from a C2 row (Phase 11). Defaults to `false`: with no telemetry source
    // wired, the telemetry-derived inputs (m_telemetryRtt/minRtt,
    // m_telemetryLossRate) stay 0. rttHigh no longer depends on this gate —
    // Phase 06 re-pointed it at the VIDEO row — so bufferbloat detection works
    // without a C2 source.
    bool _haveTelemetry = false;
    // xbfirm's _cameraStatus: false pins the video path off while telemetry loss
    // is severe. Set false by processTelemetryTCPInfo on >1% loss; restored
    // after ABR_TELEMETRY_STABLE_TIMEOUT_MS of clean telemetry.
    bool _cameraStatus = true;

    // ── Router wiring (not xbfirm) ───────────────────────────────────────
    const QString m_iface;
    const bool m_isC2Configured;

    QElapsedTimer m_clock;

    RouterInterfaceStat m_video;   // last VIDEO row
    RouterInterfaceStat m_c2;      // last C2 row
    qint64 m_lastNetworkDataTime = 0;
    // Part B reset rate-limit. Sentinel: -ABR_RESET_RATE_LIMIT_MS, NOT 0 — with 0
    // the "now - m_lastResetMs >= WINDOW" test reads 0 as clock t=0 and silently
    // SUPPRESSES the first reset for the first 30s of instance life (a service
    // restarted because the link collapsed would miss its first reset). The
    // negative sentinel makes the first emit always pass.
    qint64 m_lastResetMs = -ABR_RESET_RATE_LIMIT_MS;

    // Camera closed loop — stored for logging only (see handleCameraReportedBitrate).
    int m_lastAchievedKbps = 0;

    QTimer *m_networkDataTimer = nullptr;
    QTimer *m_statusBroadcastTimer = nullptr;
};

#endif // XBADAPTIVEBITRATESTREAMING_H
