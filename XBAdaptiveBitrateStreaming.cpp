#include "XBAdaptiveBitrateStreaming.h"
#include <QDateTime>
#include <QDebug>
#include <QtMath>

XBAdaptiveBitrateStreaming* XBAdaptiveBitrateStreaming::_instance = nullptr;

XBAdaptiveBitrateStreaming::XBAdaptiveBitrateStreaming(QObject *parent)
    : IAdaptiveBitrateStreaming{parent}
    , m_lastAbrAction(ABR_NONE)
    , m_lastQualityChangeTime(0)
    , m_avgSiocoutq(0.0)
    , m_currentStatus(LOADING)  // Bắt đầu với LOADING khi khởi tạo ABR
    , m_previousStatus(LOADING)
    , m_consecutivePoorConditions(0)
    , m_lastStatusChangeTime(0)
    , m_networkDataTimer(nullptr)
    , m_lastNetworkDataTime(0)
    , m_isStarted(false)
    , m_statusBroadcastTimer(nullptr)
    , _lastTcpiSegsout(0)
    , _lastTelemetrySegsout(0)
    , m_telemetryRtt(0)
    , m_telemetryMinRtt(0)
    , m_telemetryLossRate(0.0)
    , m_alpha(1.0)
    , m_gainUp(1.0)
    , m_gainDown(1.0)
    , _haveTelemetry(false)
    , m_telemetryStableSince(0)
{
#ifdef XBFIRM
    JITTER_STABLE_THRESHOLD_MS         = Settings::generalSetting()->abrJitterStableThreshold();
    RTT_STABLE_THRESHOLD_MS            = Settings::generalSetting()->abrStableThreshold();
    RTT_STABLE_AFTER_DECREASE_MS       = Settings::generalSetting()->abrStableAfterDecrease();
    QUEUE_DELAY_LOW_AFTER_DECREASE_MS  = Settings::generalSetting()->abrQueuDelayLowAfterDecrease();
    TELEMETRY_STABLE_TIMEOUT_MS        = Settings::generalSetting()->abrTelemetryStableTimeout();
    TELEMETRY_KICK_BITRATE_KBPS        = Settings::generalSetting()->abrTelemetryKickBitrate();
    BITRATE_INCREASE_COOLDOWN_MS       = Settings::generalSetting()->abrBitrateIncreaseCooldown();
#else
    JITTER_STABLE_THRESHOLD_MS         = ABR_DEFAULT_JITTER_STABLE_THRESHOLD;
    RTT_STABLE_THRESHOLD_MS            = ABR_DEFAULT_STABLE_THRESHOLD;
    RTT_STABLE_AFTER_DECREASE_MS       = ABR_DEFAULT_STABLE_AFTER_DECREASE;
    QUEUE_DELAY_LOW_AFTER_DECREASE_MS  = ABR_DEFAULT_QUEUE_DELAY_LOW_AFTER_DECREASE;
    TELEMETRY_STABLE_TIMEOUT_MS        = ABR_DEFAULT_TELEMETRY_STABLE_TIMEOUT;
    TELEMETRY_KICK_BITRATE_KBPS        = ABR_DEFAULT_TELEMETRY_KICK_BITRATE;
    BITRATE_INCREASE_COOLDOWN_MS       = ABR_DEFAULT_BITRATE_INCREASE_COOLDOWN;
#endif



    _cameraStatus = true;
    _maxAbrBitrate = MAX_BITRATE_KBPS;
    m_problematicBitrateThreshold = _maxAbrBitrate;
    m_abrState.current_bitrate_kbps = 800;

    // Setup network data monitoring timer
    m_networkDataTimer = new QTimer(this);
    m_networkDataTimer->setInterval(5000);  // 5 seconds
    m_networkDataTimer->setSingleShot(false);
    connect(m_networkDataTimer, &QTimer::timeout, this, &XBAdaptiveBitrateStreaming::onNetworkDataTimeout);

    // Setup periodic status broadcast timer
    m_statusBroadcastTimer = new QTimer(this);
    m_statusBroadcastTimer->setInterval(5000);  // 5 seconds
    m_statusBroadcastTimer->setSingleShot(false);
    connect(m_statusBroadcastTimer, &QTimer::timeout, this, &XBAdaptiveBitrateStreaming::onStatusBroadcastTimeout);
}
XBAdaptiveBitrateStreaming::~XBAdaptiveBitrateStreaming()
{
    if (m_networkDataTimer) {
        m_networkDataTimer->stop();
        delete m_networkDataTimer;
        m_networkDataTimer = nullptr;
    }

    if (m_statusBroadcastTimer) {
        m_statusBroadcastTimer->stop();
        delete m_statusBroadcastTimer;
        m_statusBroadcastTimer = nullptr;
    }
}

void XBAdaptiveBitrateStreaming::handleSetMaxAbrBitrate(int maxBitrate)
{
    this->setMaxAbrBitrate(maxBitrate);
}
XBAdaptiveBitrateStreaming *XBAdaptiveBitrateStreaming::instance()
{
    if (_instance == nullptr)  _instance = new XBAdaptiveBitrateStreaming();
    return _instance;
}

void XBAdaptiveBitrateStreaming::updateAlphaValues(double alpha, double gainUp, double gainDown)
{

    m_alpha = alpha;
    m_gainUp = gainUp;
    m_gainDown = gainDown;

    _udpSocket.writeDatagram((QString("%1%2%3;%4;%5")
                                  .arg(ID_MODEM_DATA_ABR_CPA)
                                  .arg("0")
                                  .arg(QString::number(m_alpha))
                                  .arg(QString::number(m_gainUp))
                                  .arg(QString::number(m_gainDown))).toLocal8Bit(), QHostAddress::LocalHost, SERVER_UDP_WEBAPP);
}

void XBAdaptiveBitrateStreaming::processTCPInfo(const tcpInfo &tcpi)
{
    // Update last received data timestamp
    m_lastNetworkDataTime = QDateTime::currentMSecsSinceEpoch();

    bool bitrateChanged = false;

    if (haveLostPackets(tcpi)) {
        bitrateChanged = true;
    } else if (rttHigh(tcpi)) {
        bitrateChanged = true;
    } else {
        unsigned int oldBitrate = m_abrState.current_bitrate_kbps;
        checkSenderQueue(tcpi);
        bitrateChanged = (oldBitrate != m_abrState.current_bitrate_kbps);
    }

    // Update UX status based on network conditions and ABR decisions
    updateStreamingStatus(tcpi, bitrateChanged);
}
void XBAdaptiveBitrateStreaming::processTCPInfo(quint32 rtt, quint32 min_rtt, double throughput, double loss_rate, quint32 siocoutq, quint32 so_sndbuf, int tcpiSegsout)
{
    // Tính delta cho các giá trị cumulative
    int deltaSegsout = tcpiSegsout - _lastTcpiSegsout;
    _lastTcpiSegsout = tcpiSegsout;

    tcpInfo tcpi;
    tcpi.rtt = rtt/1000;
    tcpi.min_rtt = min_rtt/1000;
    tcpi.throughput = (throughput * 8) / 1000.0;
    // Tính loss_rate dựa trên delta (số gói tin trong khoảng thời gian này)
    tcpi.loss_rate = (deltaSegsout > 0) ? (loss_rate / deltaSegsout * 100.0) : 0.0;
    tcpi.siocoutq = siocoutq;
    tcpi.so_sndbuf = so_sndbuf;
    if (!_cameraStatus) return;
    this->processTCPInfo(tcpi);
}
void XBAdaptiveBitrateStreaming::processTCPInfo(double rtt, double deliveryRate, double tcpiRtt, int tcpiLoss, int tcpiSegsout, int tcpiSndCwnd, int tcpiSndMss, int tcpiLastAckRecv, int sockOutq, int sockSndBuf, int tcpiMinRtt)
{
    this->processTCPInfo(rtt, (double) tcpiMinRtt, deliveryRate, tcpiLoss, sockOutq, sockSndBuf, tcpiSegsout);
}

void XBAdaptiveBitrateStreaming::processTelemetryTCPInfo(double rtt, double deliveryRate, double tcpiRtt, int tcpiLoss, int tcpiSegsout, int tcpiSndCwnd, int tcpiSndMss, int tcpiLastAckRecv, int sockOutq, int sockSndBuf, int tcpiMinRtt)
{
    if (!_haveTelemetry) return;

    tcpInfo telemetry;

    int deltaSegsout      = tcpiSegsout - _lastTelemetrySegsout;
    _lastTelemetrySegsout = tcpiSegsout;
    telemetry.loss_rate   = (deltaSegsout > 0) ? (static_cast<double>(tcpiLoss) / deltaSegsout * 100.0) : 0.0;

    m_telemetryLossRate = telemetry.loss_rate;
    m_telemetryRtt      = static_cast<quint32>(rtt / 1000);
    m_telemetryMinRtt   = static_cast<quint32>(tcpiMinRtt / 1000);

    if (telemetry.loss_rate > 1.0)
    {
        if (m_abrState.current_bitrate_kbps > MIN_BITRATE_KBPS && m_abrState.current_bitrate_kbps < m_problematicBitrateThreshold)
        {
            m_problematicBitrateThreshold = m_abrState.current_bitrate_kbps;
            qWarning() << "[ABR] Kick failed: recorded problematic threshold =" << m_problematicBitrateThreshold << "kbps";
        }
        _cameraStatus = false;
        m_telemetryStableSince = 0;
        m_abrState.current_bitrate_kbps = MIN_BITRATE_KBPS;
        emit bitrateChanged(0);
        return;
    }
    else
    {
        if(!_cameraStatus)
        {
            qint64 now = QDateTime::currentMSecsSinceEpoch();

            if (m_telemetryStableSince == 0) m_telemetryStableSince = now;

            if (now - m_telemetryStableSince >= static_cast<qint64>(TELEMETRY_STABLE_TIMEOUT_MS))
            {
                _cameraStatus = true;
                m_telemetryStableSince = 0;

                if (m_abrState.current_bitrate_kbps <= MIN_BITRATE_KBPS)
                {
                    unsigned int kickBitrate;

                    if (m_problematicBitrateThreshold < _maxAbrBitrate)
                    {
                        kickBitrate = static_cast<unsigned int>(m_problematicBitrateThreshold * 0.9);
                        kickBitrate = qBound(static_cast<unsigned int>(MIN_BITRATE_KBPS + BITRATE_STEP_KBPS), kickBitrate, _maxAbrBitrate);
                        qWarning() << "[ABR] ABR Kick (smart): MIN →" << kickBitrate << "kbps (threshold:" << m_problematicBitrateThreshold << "kbps)";
                    }
                    else
                    {
                        kickBitrate = qMin(TELEMETRY_KICK_BITRATE_KBPS, _maxAbrBitrate);
                        qWarning() << "[ABR] ABR Kick (probe): MIN →" << kickBitrate << "kbps";
                    }

                    setNewBitrate(kickBitrate);
                }
                else
                {
                    qInfo() << "[ABR] ABR recovery: bitrate already at" << m_abrState.current_bitrate_kbps << "kbps, no kick needed";
                }
            }

        }
    }
}

bool XBAdaptiveBitrateStreaming::haveLostPackets(const tcpInfo &tcpi)
{
    if (tcpi.loss_rate <= 0.8) return false;

    if (m_telemetryLossRate > 0.3) {
        qWarning() << "[ABR] High video loss:" << tcpi.loss_rate << "% + Telemetry loss:" << m_telemetryLossRate << "% → Network degraded, dropping to MIN";
        setNewBitrate(MIN_BITRATE_KBPS);
    } else {
        unsigned int new_bitrate = static_cast<unsigned int>(m_abrState.current_bitrate_kbps * 0.50);
        qWarning() << "[ABR] High video loss:" << tcpi.loss_rate << "% but Telemetry OK (" << m_telemetryLossRate << "%) → Self-congestion, reducing 50% to" << new_bitrate << "kbps";
        setNewBitrate(new_bitrate);
    }
    m_rttHistory.clear();
    return true;
}
bool XBAdaptiveBitrateStreaming::rttHigh(const tcpInfo &tcpi)
{
    Q_UNUSED(tcpi);
    quint32 queuing_delay = m_telemetryRtt - m_telemetryMinRtt;
    if (queuing_delay <= 500) return false;

    qWarning() << "[ABR] High Telemetry RTT (Queuing Delay > 500ms):" << queuing_delay << "ms"  << "(RTT:" << m_telemetryRtt << "ms, MinRTT:" << m_telemetryMinRtt << "ms)";
    unsigned int new_bitrate = static_cast<unsigned int>(m_abrState.current_bitrate_kbps * 0.80);
    setNewBitrate(new_bitrate);
    m_rttHistory.clear();
    return true;

}
double XBAdaptiveBitrateStreaming::calculateJitter()
{
    if (m_rttHistory.size() < 2) return 0.0;

    double sum = 0.0;
    for (const auto &val : m_rttHistory) { sum += val; }
    double mean = sum / m_rttHistory.size();

    double sq_sum = 0.0;
    for (const auto &val : m_rttHistory) { sq_sum += qPow(val - mean, 2); }
    return qSqrt(sq_sum / m_rttHistory.size());
}
void XBAdaptiveBitrateStreaming::checkSenderQueue(const tcpInfo &tcpi)
{
    m_rttHistory.append(tcpi.rtt);
    if (m_rttHistory.size() > static_cast<int>(RTT_HISTORY_SIZE)) m_rttHistory.removeFirst();

    double jitter = calculateJitter();

    constexpr double EMA_ALPHA = 0.5;
    m_avgSiocoutq = (EMA_ALPHA * tcpi.siocoutq) + ((1.0 - EMA_ALPHA) * m_avgSiocoutq);

    unsigned int current_throughput_Bps = (tcpi.throughput * 1000) / 8;

    if (current_throughput_Bps == 0)
    {
        qWarning() << "[ABR] Throughput is 0! Dropping to MIN";
        setNewBitrate(MIN_BITRATE_KBPS);
        return;
    }

    double queuing_delay_ms = (m_avgSiocoutq / static_cast<double>(current_throughput_Bps)) * 1000.0;

    // qWarning() << "[ABR] Queued 123=" << static_cast<int>(m_avgSiocoutq) << "B, Tput=" << tcpi.throughput << "kbps, QDelay=" << queuing_delay_ms << "ms";

    double increase_q_delay_threshold   = (m_lastAbrAction == ABR_DECREASE) ? QUEUE_DELAY_LOW_AFTER_DECREASE_MS : QUEUE_DELAY_LOW_THRESHOLD_MS;
    double increase_rtt_delay_threshold = (m_lastAbrAction == ABR_DECREASE) ? RTT_STABLE_AFTER_DECREASE_MS : RTT_STABLE_THRESHOLD_MS;
    if (queuing_delay_ms > QUEUE_DELAY_HIGH_THRESHOLD_MS)
    {
        qWarning() << "[ABR] Queued time >" << QUEUE_DELAY_HIGH_THRESHOLD_MS << "ms";
        unsigned int decreased_bitrate = static_cast<unsigned int>(m_abrState.current_bitrate_kbps * m_gainDown);
        setNewBitrate(decreased_bitrate);
        m_rttHistory.clear();
    }
    else if (queuing_delay_ms < increase_q_delay_threshold)
    {
        qWarning() << "[ABR] Queued time <" << queuing_delay_ms << "ms";
        quint32 network_delay = tcpi.rtt - tcpi.min_rtt;
            bool testNetworkDelay = (network_delay < increase_rtt_delay_threshold);
        if (network_delay < increase_rtt_delay_threshold)
        {
            qWarning() << "[ABR] Network delay <" << network_delay << "ms";
            if (jitter < JITTER_STABLE_THRESHOLD_MS)
            {
                qWarning() << "[ABR] RTT stable (" << network_delay << "ms < " << increase_rtt_delay_threshold << "ms) & Jitter stable (" << jitter << "ms < " << JITTER_STABLE_THRESHOLD_MS << "ms)";
                qint64 now = QDateTime::currentMSecsSinceEpoch();
                qint64 time_since_last_change = now - m_lastQualityChangeTime;
                qWarning() << "[ABR] Cooldown check:" << time_since_last_change << "ms since last change (need >= " << BITRATE_INCREASE_COOLDOWN_MS << "ms)";
                if (time_since_last_change >= BITRATE_INCREASE_COOLDOWN_MS)
                {
                    unsigned int new_bitrate = m_abrState.current_bitrate_kbps + static_cast<unsigned int>(m_gainUp * BITRATE_STEP_KBPS);
                    setNewBitrate(new_bitrate);
                }
            }
        }
    }
    else   qWarning() << "[ABR] Another condition: " << "Queued time is moderate (" << increase_q_delay_threshold << " < " << queuing_delay_ms << "ms) < " << QUEUE_DELAY_HIGH_THRESHOLD_MS << "ms). Holding bitrate.";
    // qWarning() << "[ABR] --> Thoat vong check";
}

void XBAdaptiveBitrateStreaming::setNewBitrate(unsigned int new_bitrate_kbps)
{
    // Apply alpha coefficient (reliability factor from cellular prediction)
    double adjusted_bitrate_raw = new_bitrate_kbps * m_alpha ;

    adjusted_bitrate_raw = qBound(MIN_BITRATE_KBPS, static_cast<unsigned int>(adjusted_bitrate_raw), _maxAbrBitrate);

    // Check if change is significant enough BEFORE rounding (threshold applies to both increase and decrease)
    double diff_raw = (adjusted_bitrate_raw > m_abrState.current_bitrate_kbps) ? (adjusted_bitrate_raw - m_abrState.current_bitrate_kbps) : (m_abrState.current_bitrate_kbps - adjusted_bitrate_raw);

    if (diff_raw < MIN_BITRATE_CHANGE_THRESHOLD)
    {
        return;
    }

    /*// Round to step and bound to valid ranges
    unsigned int adjusted_bitrate = static_cast<unsigned int>(adjusted_bitrate_raw);
    adjusted_bitrate = (adjusted_bitrate / BITRATE_STEP_KBPS) * BITRATE_STEP_KBPS;
    adjusted_bitrate = qBound(MIN_BITRATE_KBPS, adjusted_bitrate, MAX_BITRATE_KBPS); */

    // Testing without rounding
    unsigned int adjusted_bitrate = static_cast<unsigned int>(adjusted_bitrate_raw);

    qInfo() << "--- [ABR] BITRATE CHANGE  --- ";
    qInfo() << "[ABR] After rounding:" << adjusted_bitrate << "kbps";
    qInfo() << "[ABR] Old:" << m_abrState.current_bitrate_kbps << "kbps -> New:" << adjusted_bitrate << "kbps";
    qInfo() << "----------------------";

    if (adjusted_bitrate < m_abrState.current_bitrate_kbps)
    {
        if (m_lastAbrAction == ABR_INCREASE || m_lastAbrAction == ABR_NONE)
        {
            // qInfo() << "ABR Hysteresis: Recording problematic threshold:" << m_abrState.current_bitrate_kbps << "kbps";
            m_problematicBitrateThreshold = m_abrState.current_bitrate_kbps;
        }
        m_lastAbrAction = ABR_DECREASE;
    }
    else if (adjusted_bitrate >= m_problematicBitrateThreshold)
    {
        // qInfo() << "ABR Hysteresis: Exceeded threshold" << m_problematicBitrateThreshold << "kbps. Resetting." << ": MAX_BITRATE_KBPS: "<<_maxAbrBitrate;
        m_problematicBitrateThreshold = _maxAbrBitrate;
        m_lastAbrAction = ABR_INCREASE;
    }
    // else    m_lastAbrAction = ABR_INCREASE;

    m_abrState.current_bitrate_kbps = adjusted_bitrate;
    m_lastQualityChangeTime = QDateTime::currentMSecsSinceEpoch();

    emit bitrateChanged(adjusted_bitrate);
}

// 🎯 UX Status Mapping Logic
void XBAdaptiveBitrateStreaming::updateStreamingStatus(const tcpInfo &tcpi, bool bitrateChanged)
{
    StreamingQualityStatus newStatus = m_currentStatus;
    qint64 now = QDateTime::currentMSecsSinceEpoch();

    // === 1. BUFFERING / RECONNECTING ===
    // Throughput = 0 hoặc packet loss cực cao → video bị đứng hình
    if (tcpi.throughput == 0.0 || tcpi.loss_rate > 5.0) {
        newStatus = BUFFERING;
        m_consecutivePoorConditions++;
    }
    // === 2. LOADING ===
    // Sau khi buffering, ABR đang warm-up lại (trong 2-3 giây đầu)
    else if (m_previousStatus == BUFFERING && (now - m_lastStatusChangeTime) < 3000) {
        newStatus = LOADING;
        m_consecutivePoorConditions = 0;
    }
    // === 3. POOR NETWORK ===
    // Bitrate rất thấp (gần MIN) hoặc đã bị giảm bitrate nhiều lần liên tiếp
    else if (m_abrState.current_bitrate_kbps <= (MIN_BITRATE_KBPS + BITRATE_STEP_KBPS) ||
             (m_lastAbrAction == ABR_DECREASE && m_consecutivePoorConditions >= 2)) {
        newStatus = POOR_NETWORK;
        if (m_lastAbrAction == ABR_DECREASE) {
            m_consecutivePoorConditions++;
        }
    }
    // === 4. OPTIMIZING ===
    // Có thay đổi bitrate (tăng hoặc giảm) trong vòng 3 giây gần đây
    else if (bitrateChanged || (now - m_lastQualityChangeTime) < 3000) {
        newStatus = OPTIMIZING;
        // Reset counter nếu bắt đầu tăng bitrate trở lại
        if (m_lastAbrAction == ABR_INCREASE) {
            m_consecutivePoorConditions = 0;
        }
    }
    // === 5. PLAYING (AUTO) ===
    // ABR ổn định, không thay đổi bitrate, mạng tốt
    else {
        newStatus = PLAYING_AUTO;
        m_consecutivePoorConditions = 0;
    }

    // Emit signal nếu trạng thái thay đổi
    if (newStatus != m_currentStatus) {
        QString statusName;
        switch (newStatus) {
        case PLAYING_AUTO: statusName = "Playing (Auto)"; break;
        case OPTIMIZING: statusName = "Optimizing"; break;
        case POOR_NETWORK: statusName = "Poor Network"; break;
        case BUFFERING: statusName = "Buffering / Reconnecting"; break;
        case LOADING: statusName = "Loading"; break;
        }

        // qInfo() << "🎨 UX Status Changed:" << statusName;

        m_previousStatus = m_currentStatus;
        m_currentStatus = newStatus;
        m_lastStatusChangeTime = now;

        emit onStatus(static_cast<int>(newStatus));
    }
}

unsigned int XBAdaptiveBitrateStreaming::maxAbrBitrate() const
{
    return _maxAbrBitrate;
}

void XBAdaptiveBitrateStreaming::setMaxAbrBitrate(unsigned int newMaxAbrBitrate)
{
    _maxAbrBitrate = newMaxAbrBitrate;
    m_problematicBitrateThreshold = _maxAbrBitrate;
}

void XBAdaptiveBitrateStreaming::reset(int bitrateKbps)
{
    _cameraStatus = true;
    // Reset core ABR state
    m_abrState.current_bitrate_kbps = bitrateKbps;
    m_lastAbrAction = ABR_NONE;
    // m_problematicBitrateThreshold = _maxAbrBitrate;
    m_lastQualityChangeTime = 0;
    m_avgSiocoutq = 0.0;
    m_rttHistory.clear();

    // Reset UX status
    m_currentStatus = LOADING;
    m_previousStatus = LOADING;
    m_consecutivePoorConditions = 0;
    m_lastStatusChangeTime = 0;

    // Reset network monitoring state
    m_lastNetworkDataTime = 0;

    // Reset cumulative tracking
    _lastTcpiSegsout = 0;
    _lastTelemetrySegsout = 0;

    // Reset alpha values to default
    m_alpha = 1.0;
    m_gainUp = 1.0;
    m_gainDown = 1.0;

}

void XBAdaptiveBitrateStreaming::handleChangeBitrateStep(int bitrateStep)
{
    if (_bitrateStepKps != bitrateStep) {
        _bitrateStepKps = bitrateStep;
    }
}

void XBAdaptiveBitrateStreaming::start()
{
    if (m_isStarted) {
        return;
    }

    // Reset ABR state before starting
    reset(1000);

    // qInfo() << "🚀 XBAdaptiveBitrateStreaming::start() - Starting network monitoring";
    m_isStarted = true;
    m_lastNetworkDataTime = QDateTime::currentMSecsSinceEpoch();

    // Start the timer to monitor network data
    if (m_networkDataTimer) {
        m_networkDataTimer->start();
    }

    // Start the periodic status broadcast timer
    if (m_statusBroadcastTimer) {
        m_statusBroadcastTimer->start();
    }
}

void XBAdaptiveBitrateStreaming::stop()
{
    if (!m_isStarted) {
        return;
    }

    // qInfo() << "🛑 XBAdaptiveBitrateStreaming::stop() - Stopping network monitoring";
    m_isStarted = false;

    // Stop the timer
    if (m_networkDataTimer && m_networkDataTimer->isActive()) {
        m_networkDataTimer->stop();
    }

    // Stop the status broadcast timer
    if (m_statusBroadcastTimer && m_statusBroadcastTimer->isActive()) {
        m_statusBroadcastTimer->stop();
    }

    // Reset state
    m_lastNetworkDataTime = 0;
    m_rttHistory.clear();
    m_consecutivePoorConditions = 0;
}

void XBAdaptiveBitrateStreaming::onNetworkDataTimeout()
{
    if (!m_isStarted) {
        return;
    }

    qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 timeSinceLastData = now - m_lastNetworkDataTime;

    // If no data received for more than 5 seconds, request connection stats
    if (timeSinceLastData >= 5000) {
        // qWarning() << "⚠️ No network data received for 5+ seconds! Requesting connection stats...";
        emit requestStartConnectionStats();
    }
}

void XBAdaptiveBitrateStreaming::onStatusBroadcastTimeout()
{
    if (!m_isStarted) {
        return;
    }

    // Định kỳ broadcast status hiện tại mỗi 5 giây để GCS luôn nhận được
    emit onStatus(static_cast<int>(m_currentStatus));
}

void XBAdaptiveBitrateStreaming::handleSerialStatus(bool isConnected)
{
    if (_haveTelemetry != isConnected) {
        _haveTelemetry = isConnected;
        if (!_haveTelemetry && !_cameraStatus) {
            _cameraStatus = true;
            // qWarning() << "Telemetry no longer available but _cameraStatus was false. Reset video streaming";
            reset(500);
        }
    }
}
