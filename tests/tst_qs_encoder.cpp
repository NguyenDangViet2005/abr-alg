// Unit test for the QS wire codec, dev/QoSDebugProtocol.*.
//
// No Qt Test framework (the repo has none): a plain main() with asserts and a
// non-zero exit on failure, so it can be compiled and run alone.
//
// Build (adjust include paths for your Qt):
//   g++ -std=c++17 -I.. -I$QT_INC -I$QT_INC/QtCore \
//       tst_qs_encoder.cpp ../dev/QoSDebugProtocol.cpp \
//       -lQt5Core -o tst_qs_encoder
//
// The project rule is that the user builds; this file only has to be correct.
//
// WHY THIS EXISTS: a C++ encode->decode round-trip CANNOT catch a magic that
// is byte-swapped on the wire, because the same QDataStream reads it back the
// same way. The magic assertions below are deliberately BYTE-LITERAL ('Q','S')
// so they agree with the dashboard's parser.js, which reads raw bytes. That
// cross-language agreement is the whole point.

#include "../dev/QoSDebugProtocol.h"   // TREND_* and the QS_BR_*/QS_R_* wire values
#include <QByteArray>
#include <QDebug>

static int g_failures = 0;

static void expectTrue(bool ok, const char *what)
{
    if (!ok) { ++g_failures; qWarning().nospace() << "FAIL " << what; }
    else     { qInfo().nospace()    << "ok   " << what; }
}

static void expectEq(int got, int want, const char *what)
{
    if (got != want) {
        ++g_failures;
        qWarning().nospace() << "FAIL " << what << ": got " << got << ", want " << want;
    } else {
        qInfo().nospace() << "ok   " << what << " = " << got;
    }
}

// A representative SAMPLE with every field set to a distinctive value.
static QsSample makeSample()
{
    QsSample s;
    s.timestampMs = 1234567890123LL;
    s.tickSeq = 42;
    s.iface = "wwan0";
    s.priority = 1;
    s.isActivated = true;
    s.isConnected = true;
    s.isMptcp = true;
    s.usable = false;
    s.videoPresent = true;
    s.c2Present = true;
    s.videoTrend = TREND_STABLE;
    s.c2Trend = TREND_INCREASE;
    s.videoQuality = 255;
    s.c2Quality = 255;
    s.c2Mode = 0;                 // FRESH
    s.abrBranch = QS_BR_HALVE;
    s.abrReason = QS_R_LOSS_HIGH;
    s.notUsableTicks = 0;
    s.bitrateKbps = 1000;
    s.maxAbrKbps = 6000;
    s.ceilingKbps = 4800;         // v2: learned ceiling (0 would mean "not found")
    s.capKbps = 3840;             // v3: headroom cap = 0.8 × ceiling (always real)
    s.sinceLastChangeMs = 2000;
    s.video = QsFlow{ 90.0, 3000.0, 1.5, 30.0, 20.0, 10.0, 3.0 };
    s.c2    = QsFlow{ 88.0, 2500.0, 0.1, 40.0, 25.0, 5.0, 2.0 };
    return s;
}

int main()
{
    // ── header magic: raw 'Q','S', NOT a little-endian u16 ───────────────
    {
        const QByteArray dg = buildQsDatagram(QS_SAMPLE, QByteArray(1, '\0'));
        expectTrue(dg.size() >= QS_HEADER_SIZE, "datagram >= header size");
        expectEq(static_cast<quint8>(dg.at(0)), 0x51, "magic byte 0 = 'Q'");
        expectEq(static_cast<quint8>(dg.at(1)), 0x53, "magic byte 1 = 'S'");
        expectEq(static_cast<quint8>(dg.at(2)), QS_VERSION, "version byte");
        expectEq(static_cast<quint8>(dg.at(3)), QS_SAMPLE, "recordType byte");
        expectTrue(isQsDatagram(dg), "isQsDatagram accepts own output");
        expectEq(qsRecordType(dg), QS_SAMPLE, "qsRecordType reads SAMPLE");
        // A byte-swapped magic must be rejected — this is the exact defect the
        // test guards: 0x53 0x51 ('S','Q') is NOT a QS datagram.
        QByteArray swapped = dg;
        swapped[0] = 0x53;
        swapped[1] = 0x51;
        expectTrue(!isQsDatagram(swapped), "byte-swapped magic rejected");
    }

    // ── SAMPLE round-trip ────────────────────────────────────────────────
    {
        const QsSample in = makeSample();
        const QByteArray payload = encodeQsSample(in);
        expectEq(payload.size(), 106, "SAMPLE payload = 106 bytes (v3: +capKbps)");

        QsSample out;
        expectTrue(decodeQsSample(payload, out), "decodeQsSample ok");
        expectTrue(out.timestampMs == in.timestampMs, "ts round-trips");
        expectTrue(out.tickSeq == in.tickSeq, "tickSeq round-trips");
        expectTrue(out.iface == in.iface, "iface round-trips");
        expectTrue(out.isActivated == in.isActivated, "flag act round-trips");
        expectTrue(out.usable == in.usable, "flag usable round-trips");
        expectTrue(out.c2Present == in.c2Present, "c2Present round-trips");
        expectTrue(out.c2Mode == in.c2Mode, "c2Mode round-trips");
        expectTrue(out.abrBranch == in.abrBranch, "abrBranch round-trips");
        expectTrue(out.abrReason == in.abrReason, "abrReason round-trips");
        expectTrue(out.bitrateKbps == in.bitrateKbps, "bitrate round-trips");
        expectTrue(out.ceilingKbps == in.ceilingKbps, "ceilingKbps round-trips");
        expectTrue(out.capKbps == in.capKbps, "capKbps round-trips");
        expectTrue(out.video.score == in.video.score, "video.score round-trips");
        expectTrue(out.video.qdelay == in.video.qdelay, "video.qdelay round-trips");
        expectTrue(out.c2.jitter == in.c2.jitter, "c2.jitter round-trips");
        // Single-precision: a value needing >24 bits must survive as a float.
        expectTrue(out.video.thr == 3000.0, "video.thr exact");
    }

    // ── EVENT round-trip ─────────────────────────────────────────────────
    {
        QsEvent in;
        in.timestampMs = 42;
        in.iface = "wwan0";
        in.eventType = QS_EVT_SOCKET_RESET_REQUEST;
        in.detail = QStringLiteral("stuck after lowered bitrate");
        const QByteArray payload = encodeQsEvent(in);

        QsEvent out;
        expectTrue(decodeQsEvent(payload, out), "decodeQsEvent ok");
        expectTrue(out.eventType == in.eventType, "eventType round-trips");
        expectTrue(out.detail == in.detail, "event detail (u16) round-trips");
    }

    // ── CONFIG round-trip ────────────────────────────────────────────────
    {
        QsConfig in;
        in.appVersion = "1.4.2";
        in.primaryIface = "wwan0";
        in.udpPort = 9999;
        in.ifaces = { { "wwan0", 1 }, { "wwan1", 2 } };
        const QByteArray payload = encodeQsConfig(in);

        QsConfig out;
        expectTrue(decodeQsConfig(payload, out), "decodeQsConfig ok");
        expectTrue(out.appVersion == in.appVersion, "appVersion round-trips");
        expectTrue(out.primaryIface == in.primaryIface, "primaryIface round-trips");
        expectEq(out.udpPort, 9999, "udpPort round-trips");
        expectEq(out.ifaces.size(), 2, "iface count round-trips");
        if (out.ifaces.size() == 2) {
            expectTrue(out.ifaces[0].first == "wwan0", "iface[0] name");
            expectEq(out.ifaces[0].second, 1, "iface[0] priority");
        }
    }

    // ── truncation is rejected, not silently accepted ────────────────────
    {
        const QByteArray payload = encodeQsSample(makeSample());
        QsSample out;
        expectTrue(!decodeQsSample(payload.left(20), out), "truncated SAMPLE rejected");
    }

    if (g_failures == 0) {
        qInfo() << "ALL PASS";
        return 0;
    }
    qWarning() << g_failures << "FAILURE(S)";
    return 1;
}
