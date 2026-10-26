#include <QtCore>
#include <QtNetwork>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "dev/telemetry/MavlinkStream.h"
#include "dev/telemetry/C2TelemetryDecoder.h"
#include "dev/XbTelemetryHandler.h"
// Existing engine has no test seam for its final bitrate writer.
#define private public
#include "SRTAdaptiveBitrateStreaming.h"
#undef private
#include "ABRFactory.h"

static int checks = 0;
static void check(bool condition, const char *message)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static void be16(QByteArray &bytes, quint16 value) { char b[2]; qToBigEndian(value, b); bytes.append(b, 2); }
static void be32(QByteArray &bytes, quint32 value) { char b[4]; qToBigEndian(value, b); bytes.append(b, 4); }
static QByteArray packet(const QByteArray &payload, bool reverse = false, quint16 remote = 51226, int tags = 0)
{
    QByteArray ethernet(12, 0);
    for (int i = 0; i < tags; ++i) { be16(ethernet, 0x8100); be16(ethernet, 1); }
    be16(ethernet, 0x0800);
    QByteArray ip;
    ip.append(char(0x45)); ip.append(char(0)); be16(ip, quint16(28 + payload.size()));
    be16(ip, 0); be16(ip, 0); ip.append(char(64)); ip.append(char(17)); be16(ip, 0);
    be32(ip, reverse ? 0xc0a8901e : 0xc0a89073); be32(ip, reverse ? 0xc0a89073 : 0xc0a8901e);
    be16(ip, reverse ? remote : 3345); be16(ip, reverse ? 3345 : remote);
    be16(ip, quint16(payload.size() + 8)); be16(ip, 0);
    return ethernet + ip + payload;
}
static QByteArray data(quint32 seq, const QByteArray &body, quint32 sid = 9,
                       quint32 position = 3, bool retransmit = false, quint16 remote = 51226,
                       bool reverse = false, quint32 extraFlags = 0)
{
    QByteArray bytes;
    be32(bytes, seq); be32(bytes, (position << 30) | (1u << 29) | (quint32(retransmit) << 26) | 1 | extraFlags);
    be32(bytes, 0); be32(bytes, sid);
    return packet(bytes + body, reverse, remote);
}
static QByteArray wrapped(const QByteArray &body)
{
    return QByteArray::fromHex("fd0000000163") + body;
}
static QByteArray ack(quint32 rtt = 4000, quint32 sid = 10, quint16 remote = 51226, bool light = false)
{
    QByteArray bytes;
    be32(bytes, 0x80020000); be32(bytes, 1); be32(bytes, 0); be32(bytes, sid); be32(bytes, 2);
    if (!light) { be32(bytes, rtt); be32(bytes, 500); be32(bytes, 8181); }
    return packet(bytes, true, remote);
}
static QJsonObject readJson(const QString &path)
{
    QFile file(path); check(file.open(QIODevice::ReadOnly), "Open independent fixture");
    QJsonParseError error;
    const auto result = QJsonDocument::fromJson(file.readAll(), &error);
    check(error.error == QJsonParseError::NoError, "Parse independent fixture");
    return result.object();
}
static C2TelemetryStat healthy()
{
    C2TelemetryStat s;
    s.captureAvailable = s.telemetryValid = s.heartbeatValid = s.peerAckValid = true;
    s.rttValid = s.baselineValid = true;
    s.telemetryAgeMs = s.heartbeatAgeMs = s.ackAgeMs = s.rttAgeMs = 0;
    s.rttMs = s.rttSmoothedMs = s.rttBaselineMs = 4;
    s.rttInflationMs = 0; s.rttRatio = 1; s.dataPackets = 50;
    s.retransmissionPercent = 0; s.windowSeconds = .5;
    return s;
}

static void protocolTests(const QString &fixtures)
{
    const auto reference = readJson(fixtures + "/mavlink-reference-frames.json")["frames"].toArray();
    check(reference.size() == 301, "301 independent message definitions");
    for (const auto &item : reference) {
        const auto object = item.toObject();
        const auto raw = QByteArray::fromHex(object["frame"].toString().toLatin1());
        MavlinkStream parser;
        const auto frames = parser.feed(raw);
        check(frames.size() == 1 && frames[0].messageId == quint32(object["id"].toInt()) &&
              frames[0].systemId == 1 && frames[0].componentId == 1, "Independent MAVLink CRC/frame identity");
    }
    const auto hb = readJson(fixtures + "/heartbeat-reference-frames.json");
    const QByteArray raw = QByteArray::fromHex(hb["normal"].toString().toLatin1());
    const QByteArray status = QByteArray::fromHex("fd00000000300000");
    const QByteArray other = QByteArray::fromHex("fd0000000273") + "OTHER";
    {
        MavlinkStream parser;
        for (const char *name : {"v1", "signed"}) {
            const auto decoded = parser.feed(QByteArray::fromHex(hb[name].toString().toLatin1()));
            check(decoded.size() == 1 && decoded[0].messageId == 0, "MAVLink1 and signed framing");
        }
        QByteArray bad = raw; bad[bad.size() - 1] = char(quint8(bad.back()) ^ 1);
        check(parser.feed(bad + raw).size() == 1 && parser.crcRejected() >= 1, "CRC rejection and resync");
    }
    {
        C2TelemetryDecoder decoder;
        decoder.ingest(data(100, wrapped(raw.left(5))), 1000);
        decoder.ingest(data(102, wrapped(raw.mid(12))), 1010);
        decoder.ingest(data(101, wrapped(raw.mid(5, 7))), 1020);
        decoder.ingest(data(102, wrapped(raw.mid(12)), 9, 3, true), 1030);
        const auto row = decoder.snapshot(2000).front();
        check(row.telemetryValid && row.heartbeatValid && row.mavlinkFramesPerSecond == 1, "Reorder and dedup across MAVLink chunks");
        check(row.dataPackets == 4 && row.retransmissions == 1 && row.retransmissionPercent == 25, "Retransmission transmission-share semantics");
        check(std::abs(row.appRxModelKiBps - (2 * raw.size() + 3) / 1024.0) < 1e-9, "App duplicate-count model");
    }
    {
        C2TelemetryDecoder decoder;
        const auto body = wrapped(raw);
        decoder.ingest(data(0x7fffffff, body.left(7), 9, 2), 1000);
        decoder.ingest(data(0, body.mid(7), 9, 1), 1100);
        const auto row = decoder.snapshot(2000).front();
        check(row.telemetryValid && row.captureGaps == 0, "SRT fragmentation and 31-bit wrap");
    }
    {
        C2TelemetryDecoder decoder;
        decoder.ingest(data(1, wrapped(raw.left(8))), 1000);
        decoder.ingest(data(2, other), 1010);
        decoder.ingest(data(3, wrapped(raw.mid(8))), 1020);
        check(decoder.snapshot(1100).front().telemetryValid, "XB internal message does not corrupt MAVLink stream");
        decoder.ingest(data(4, status), 4100);
        decoder.ingest(ack(), 4200);
        const auto row = decoder.snapshot(4300).front();
        check(!row.telemetryValid && !row.heartbeatValid && row.peerAckValid && row.rttValid,
              "Cable unplug detected even while ACK/internal messages continue");
        check(row.mavlinkKiBps == 0 && row.mavlinkFramesPerSecond == 0 && !row.inputsReady(), "No phantom FC measurements");
        decoder.ingest(data(5, wrapped(raw)), 4400);
        check(decoder.snapshot(4500).front().telemetryValid, "FC signal recovers automatically");
    }
    {
        C2TelemetryDecoder decoder;
        C2TelemetryStat row;
        for (int i = 1; i <= 5; ++i) {
            decoder.ingest(data(i, wrapped(raw)), i * 1000);
            decoder.ingest(ack(), i * 1000 + 100);
            row = decoder.snapshot(i * 1000 + 200).front();
        }
        check(row.inputsReady() && row.rttBaselineMs == 4 && row.rttInflationMs == 0, "RTT baseline warmup and ACK units");
        decoder.ingest(data(6, wrapped(raw)), 6000);
        decoder.ingest(ack(40000), 6100);
        row = decoder.snapshot(6200).front();
        check(row.rttMs == 40 && row.rttSmoothedMs > 4 && row.rttSmoothedMs < 40 &&
              row.rttInflationMs > 0 && row.rttTrendValid && row.rttTrendMsPerSecond > 0, "RTT EWMA inflation and trend");
        check(!decoder.snapshot(6400).front().rttTrendValid, "Same ACK is not a new trend sample");
        check(!decoder.snapshot(9000).front().rttValid, "RTT stale invalidation");
        decoder.ingest(data(1, wrapped(raw), 11), 10000);
        check(!decoder.snapshot(10050).front().peerAckValid, "New SRT socket invalidates reused reverse ACK");
        decoder.ingest(ack(20000), 10100);
        row = decoder.snapshot(10200).front();
        check(row.rttSmoothedMs == 20 && !row.baselineValid, "Reconnect does not inherit old RTT baseline");
        decoder.ingest(data(7, wrapped(raw), 9, 3, true), 10250);
        check(decoder.snapshot(10300).front().socketId == 11, "Late old session cannot replace current socket");
    }
    {
        C2TelemetryDecoder decoder;
        decoder.ingest(data(1, wrapped(raw.left(8))), 1000);
        decoder.ingest(data(3, wrapped(raw)), 1010);
        const auto row = decoder.snapshot(2000).front();
        check(row.captureGaps == 1 && row.telemetryValid && !row.observationOk, "Gap drops partial MAVLink and marks observation incomplete");
        decoder.addCaptureDrops(3);
        const auto drops = decoder.snapshot(2100).front();
        check(drops.captureDrops == 3 && !drops.observationOk, "Kernel capture drops invalidate observation");
        check(decoder.snapshot(2200).front().observationOk, "Capture drop counters are per window");
    }
    {
        C2TelemetryDecoder decoder;
        decoder.ingest(data(1, wrapped(raw), 9, 3, false, 64785), 1000);
        decoder.ingest(data(1, wrapped(raw), 9, 3, false, 60800), 5000);
        const auto rows = decoder.snapshot(5100);
        check(rows.size() == 1 && rows[0].destination.endsWith(":60800"), "Historical remote ports filtered");
        const auto stale = decoder.snapshot(9000);
        check(stale.size() == 1 && !stale[0].telemetryValid, "Outage remains visible when all ports stale");
        decoder.ingest(data(1, wrapped(raw), 9, 3, false, 60801), 9100);
        decoder.ingest(data(2, wrapped(raw), 9, 3, false, 60800), 9200);
        check(decoder.snapshot(9300).size() == 2, "Simultaneous fresh telemetry connections retained");
    }
    {
        C2TelemetryDecoder decoder;
        const auto untagged = data(1, wrapped(raw));
        const auto payload = untagged.mid(42);
        decoder.ingest(packet(payload, false, 51226, 2), 1000);
        check(decoder.snapshot(1100).front().telemetryValid, "Nested VLAN decoding");
        C2TelemetryDecoder truncated;
        truncated.ingest(untagged.left(untagged.size() - 1), 1000);
        check(truncated.snapshot(1100).isEmpty(), "Truncated Ethernet/IP rejected");
        C2TelemetryDecoder wrongFc(3345, 2, 1);
        wrongFc.ingest(untagged, 1000);
        check(!wrongFc.snapshot(1100).front().telemetryValid, "Wrong FC identity does not establish signal");
        C2TelemetryDecoder reverse;
        reverse.ingest(data(1, QByteArray::fromHex("fd680000000163") + raw, 9, 3, false, 51226, true), 1000);
        check(reverse.snapshot(1100).isEmpty(), "GCS direction cannot establish outbound FC signal");
        C2TelemetryDecoder encrypted;
        encrypted.ingest(data(1, wrapped(raw), 9, 3, false, 51226, false, 1u << 27), 1000);
        const auto crypt = encrypted.snapshot(1100).front();
        check(!crypt.telemetryValid && !crypt.observationOk, "Encrypted SRT never decoded as FC data");
        C2TelemetryDecoder light;
        light.ingest(data(1, wrapped(raw)), 1000); light.ingest(ack(0, 10, 51226, true), 1050);
        const auto feedback = light.snapshot(1100).front();
        check(feedback.peerAckValid && !feedback.rttValid, "Light ACK is fresh feedback without RTT");
        QByteArray nak;
        for (quint32 word : {0x80030000u, 0u, 0u, 10u, 1u}) be32(nak, word);
        light.ingest(packet(nak, true), 1150);
        const auto reports = light.snapshot(1200).front();
        check(reports.nakReports == 1 && std::abs(reports.nakReportsPerSecond - 10) < 1e-9,
              "Reverse SRT NAK reports use window duration");
        check(light.snapshot(1300).front().nakReports == 0, "NAK counts are not cumulative");
    }
}

static void policyTests()
{
    C2TelemetryPolicy policy;
    check(policy.limit(6000, 0, 0) == 50, "Unknown C2 only permits survival bootstrap");
    check(policy.limit(6000, 1000, 0) == 1000 && policy.limit(200, 1000, 0) == 200,
          "Missing C2 freezes increases but permits camera-driven reductions");
    auto sample = healthy();
    for (int now = 0; now <= 3000; now += 500) { policy.update({sample}, now); policy.evaluate(now); }
    check(policy.limit(6000, 1000, 3000) == 6000, "Healthy C2 releases recovery gate");
    sample.rttSmoothedMs = 100;
    policy.update({sample}, 3500);
    check(policy.limit(6000, 6000, 3500) == C2_HIGH_VIDEO_CEILING_KBPS, "C2 latency limits video bitrate");
    sample.rttSmoothedMs = 400;
    policy.update({sample}, 4000);
    check(policy.limit(6000, 1800, 4000) == C2_CRITICAL_VIDEO_CEILING_KBPS, "Critical C2 latency immediately squeezes video");
    sample = healthy();
    policy.update({sample}, 4500);
    check(policy.limit(6000, 200, 4500) == 200, "No immediate bitrate jump on recovery");
    for (int now = 5000; now <= 7500; now += 500) { policy.update({sample}, now); policy.evaluate(now); }
    check(policy.limit(6000, 200, 7500) == 6000, "Stable recovery releases C2 cap");
    check(policy.limit(6000, 1000, 10000) == 1000, "Stalled collector cannot keep inputs fresh");
    policy.reset(); sample.retransmissionPercent = 10;
    policy.update({sample}, 0);
    check(policy.evaluate(0).level == C2TelemetryPolicy::Critical, "Observed retransmission ratio triggers priority");
    policy.reset(); sample.dataPackets = 2;
    policy.update({sample}, 0);
    check(policy.evaluate(0).level == C2TelemetryPolicy::Normal, "Tiny retransmission sample does not imply congestion");
    policy.reset(); sample = healthy(); sample.nakReportsPerSecond = 4;
    policy.update({sample}, 0);
    check(policy.evaluate(0).level == C2TelemetryPolicy::High, "NAK report rate is an independent signal");
    policy.reset(); sample = healthy(); sample.observationOk = false;
    policy.update({sample}, 0);
    check(!policy.evaluate(0).inputsReady && policy.limit(6000, 1000, 0) == 1000, "Incomplete capture cannot approve bitrate increases");
    policy.reset();
    sample = healthy(); sample.rttSmoothedMs = 100;
    policy.update({sample}, 0); policy.evaluate(0);
    sample = healthy(); policy.update({sample}, 500); policy.evaluate(500);
    sample.rttSmoothedMs = 100; policy.update({sample}, 1000); policy.evaluate(1000);
    sample = healthy();
    for (int now = 1500; now <= 4000; now += 500) { policy.update({sample}, now); policy.evaluate(now); }
    check(policy.evaluate(4000).level == C2TelemetryPolicy::High, "Unstable recovery restarts continuous stability timer");
    policy.update({sample}, 4500);
    check(policy.evaluate(4500).level == C2TelemetryPolicy::Normal, "Three continuous healthy seconds release the cap");
}

static void engineTests()
{
    SRTAdaptiveBitrateStreaming abr;
    abr.start();
    SiyiLinkStat rf; rf.isAlive = true; rf.rssi = -62; rf.signal = 95;
    abr.handleSiyiLinkStat(rf);
    abr.applyNewBitrate(6000, 10, 50, 0);
    check(abr.m_currentBitrateKbps == 6000, "Legacy ABR works before native mode is enabled");
    abr.setNativeC2TelemetryEnabled(true);
    auto sample = healthy(); sample.rttSmoothedMs = 400;
    abr.handleNativeC2Telemetry({sample});
    check(abr.m_currentBitrateKbps == C2_CRITICAL_VIDEO_CEILING_KBPS &&
          abr.m_c2Priority == SRTAdaptiveBitrateStreaming::C2PriorityLevel::Critical,
          "Native decoder contract reaches final ABR writer");
    abr.applyNewBitrate(6000, 10, 50, 0);
    check(abr.m_currentBitrateKbps == C2_CRITICAL_VIDEO_CEILING_KBPS, "Camera/RF cannot bypass C2 cap");
    SRTPeerStat legacy; legacy.msRTT = 1;
    abr.handleC2ConnectionStats({legacy});
    check(abr.m_c2Priority == SRTAdaptiveBitrateStreaming::C2PriorityLevel::Critical,
          "Legacy 9991 cannot overwrite native C2 state");
    abr.reset(6000);
    check(abr.m_currentBitrateKbps == C2_CRITICAL_VIDEO_CEILING_KBPS, "Video reset cannot bypass native C2 cap");
    abr.handleSiyiLinkStat(rf);
    abr.handleSiyiLinkLost();
    const auto before = abr.m_currentBitrateKbps;
    abr.applyNewBitrate(50, 10, 50, 0);
    check(abr.m_currentBitrateKbps == before, "SIYI pause remains authoritative");
    abr.setNativeC2TelemetryEnabled(false);
    abr.handleSiyiLinkStat(rf);
    abr.applyNewBitrate(1000, 10, 50, 0);
    abr.setNativeC2TelemetryEnabled(true);
    auto missing = healthy(); missing.telemetryValid = false; missing.telemetryAgeMs = 3000;
    abr.handleNativeC2Telemetry({missing});
    abr.applyNewBitrate(6000, 10, 50, 0);
    check(abr.m_currentBitrateKbps == 1000, "Live ACKs with missing FC cannot approve an increase");
    abr.applyNewBitrate(200, 10, 50, 0);
    check(abr.m_currentBitrateKbps == 200, "Missing FC still permits camera-driven reductions");
    abr.stop();
    ABRFactory factory;
    factory.startCameraSocketAbr();
    factory.srtAbr()->setNativeC2TelemetryEnabled(true);
    factory.srtAbr()->handleSiyiLinkStat(rf);
    factory.handleNativeC2Data({sample});
    check(factory.srtAbr()->m_c2Priority == SRTAdaptiveBitrateStreaming::C2PriorityLevel::Critical,
          "Factory signal/slot wiring forwards typed native C2 data");
    factory.stopCameraSocketAbr();
}

static void captureFailureTest()
{
    XbTelemetryHandler capture;
    int errors = 0, updates = 0;
    bool available = true, signal = true;
    QObject::connect(&capture, &XbTelemetryHandler::captureError, [&](const QString &) { ++errors; });
    QObject::connect(&capture, &XbTelemetryHandler::telemetryUpdated, [&](const QVector<C2TelemetryStat> &samples) {
        ++updates;
        available = samples.front().captureAvailable;
        signal = samples.front().telemetryValid;
    });
    check(!capture.start("xb_missing_test"), "Unavailable capture does not claim startup success");
    QEventLoop loop;
    QTimer::singleShot(650, &loop, &QEventLoop::quit);
    loop.exec();
    capture.stop();
    check(errors == 1 && updates >= 1 && !available && !signal, "Capture failure publishes unavailable C2 inputs");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    check(argc == 2, "Pass tests/telemetry fixture directory");
    protocolTests(QString::fromLocal8Bit(argv[1]));
    policyTests();
    engineTests();
    captureFailureTest();
    std::printf("PASS: %d checks (301 MAVLink messages, SRT sessions, outage/recovery, C2 policy and ABR wiring)\n", checks);
}
