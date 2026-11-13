#include <QtTest>
#include "../SRTAdaptiveBitrateStreaming.h"
class RfFallbackTest : public QObject {
    Q_OBJECT
private slots:
    void initialBitrateUsesHealthyQos_data() {
        QTest::addColumn<double>("bandwidthMbps");
        QTest::addColumn<double>("rttMs");
        QTest::addColumn<uint>("expectedKbps");
        QTest::newRow("healthy") << 4.0 << 7.0 << 1000u;
        QTest::newRow("bandwidth-limited") << 0.7 << 7.0 << 600u;
        QTest::newRow("missing-bandwidth") << 0.0 << 7.0 << 100u;
        QTest::newRow("high-rtt") << 4.0 << 100.0 << 100u;
    }

    void initialBitrateUsesHealthyQos() {
        QFETCH(double, bandwidthMbps);
        QFETCH(double, rttMs);
        QFETCH(uint, expectedKbps);
        SRTAdaptiveBitrateStreaming abr;
        QSignalSpy changes(&abr, &IAdaptiveBitrateStreaming::bitrateChanged);
        abr.start();

        SiyiLinkStat rf;
        rf.isAlive = true;
        rf.signal = 100;
        rf.rssi = -30;
        abr.handleSiyiLinkStat(rf);

        SRTPeerStat peer;
        peer.msRTT = rttMs;
        peer.mbpsBandwidth = bandwidthMbps;
        peer.mbpsSendRate = 0.2;
        peer.pktSentTotal = 100;
        abr.handleQosCameraConnection({peer});

        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes.first().at(0).toUInt(), expectedKbps);
    }

    void clearStateRespectsBandwidthCap() {
        SRTAdaptiveBitrateStreaming abr;
        QSignalSpy changes(&abr, &IAdaptiveBitrateStreaming::bitrateChanged);
        abr.setMaxAbrBitrate(400);
        abr.start();

        SiyiLinkStat rf;
        rf.isAlive = true;
        rf.signal = 100;
        rf.rssi = -30;
        SRTPeerStat peer;
        peer.peerAddress = "192.168.144.30";
        peer.peerPort = 49975;
        peer.msRTT = 7;
        peer.mbpsSendRate = 0.2;

        const auto sample = [&](double bandwidth) {
            QTest::qWait(250);
            abr.handleSiyiLinkStat(rf);
            peer.mbpsBandwidth = bandwidth;
            peer.pktSentTotal += 100;
            abr.handleQosCameraConnection({peer});
        };
        const auto bitrate = [&]() {
            return changes.isEmpty() ? 0u : changes.last().at(0).toUInt();
        };

        for (int i = 0; i < 60 && bitrate() < 400; ++i) sample(0.5);
        QCOMPARE(bitrate(), 400u);

        // A lone estimate dip can put the moving average below the cap.
        for (int i = 0; i < 5; ++i) sample(0.5);
        sample(0.2);
        QCOMPARE(bitrate(), 400u);
        sample(0.5);
        QCOMPARE(bitrate(), 400u);

        // RTT and loss stay clean while the sustained bandwidth cap drops.
        for (int i = 0; i < 12; ++i) sample(0.25);
        QCOMPARE(bitrate(), 200u);
        for (int i = 0; i < 10; ++i) sample(0.25);
        QCOMPARE(bitrate(), 200u);
    }

    void strongerRfAcceleratesOnlyWithCleanQos_data() {
        QTest::addColumn<double>("bandwidthMbps");
        QTest::addColumn<bool>("packetLoss");
        QTest::addColumn<uint>("expectedFirstIncrease");
        QTest::newRow("healthy") << 5.0 << false << 1250u;
        QTest::newRow("bandwidth-limited") << 1.0 << false << 500u;
        QTest::newRow("loss-after-rf-recovery") << 5.0 << true << 450u;
    }

    void strongerRfAcceleratesOnlyWithCleanQos() {
        QFETCH(double, bandwidthMbps);
        QFETCH(bool, packetLoss);
        QFETCH(uint, expectedFirstIncrease);
        SRTAdaptiveBitrateStreaming abr;
        QSignalSpy changes(&abr, &IAdaptiveBitrateStreaming::bitrateChanged);
        abr.start();

        SiyiLinkStat rf;
        rf.isAlive = true;
        rf.signal = 100;
        rf.rssi = -83;
        SRTPeerStat peer;
        peer.msRTT = 7;
        peer.mbpsBandwidth = bandwidthMbps;
        peer.mbpsSendRate = 0.25;

        const auto sample = [&]() {
            abr.handleSiyiLinkStat(rf);
            peer.pktSentTotal += 100;
            abr.handleQosCameraConnection({peer});
        };
        for (int i = 0; i < 5; ++i) sample();
        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes.last().at(0).toUInt(), 250u);

        rf.rssi = -55;
        for (int i = 0; i < 3; ++i) abr.handleSiyiLinkStat(rf);
        QCOMPARE(changes.size(), 1);
        if (packetLoss) ++peer.pktSndLossTotal;

        for (int i = 0; i < 28 && changes.size() == 1; ++i) {
            sample();
            if (changes.size() == 1) QTest::qWait(200);
        }
        QVERIFY(changes.size() > 1);
        QCOMPARE(changes.at(1).at(0).toUInt(), expectedFirstIncrease);
    }

    void invalidRfKeepsQosAndWatchdogActive() {
        SRTAdaptiveBitrateStreaming abr;
        QSignalSpy changes(&abr, &IAdaptiveBitrateStreaming::bitrateChanged);
        QSignalSpy priority(&abr, &IAdaptiveBitrateStreaming::c2PriorityChanged);
        abr.start();
        abr.onHeartbeatTimeout();
        QCOMPARE(changes.size(), 0); // No camera QoS: no fabricated startup.
        SiyiLinkStat rf;
        rf.isAlive = true; rf.signal = 100; rf.rssi = 3;
        abr.handleSiyiLinkStat(rf);
        SRTPeerStat p;
        p.peerAddress = "192.168.144.30"; p.peerPort = 49975;
        p.msRTT = 7; p.mbpsBandwidth = 4; p.mbpsSendRate = 0.1;
        p.pktSentTotal = 100;
        abr.handleQosCameraConnection({p});
        QVERIFY(!changes.isEmpty());
        QCOMPARE(changes.last().at(0).toUInt(), 100u);
        p.pktRetransTotal = 500; p.pktSndDropTotal = 100;
        abr.handleC2ConnectionStats({p});
        abr.onHeartbeatTimeout();
        QVERIFY(priority.isEmpty()); // Historical totals must not mean critical C2.
        QTest::qWait(2100);
        rf.rssi = 1; abr.handleSiyiLinkStat(rf);
        p.pktSentTotal += 100;
        abr.handleQosCameraConnection({p});
        for (const auto &change : changes) QVERIFY(change.at(0).toUInt() <= 100u);
        QTest::qWait(5100);
        abr.onHeartbeatTimeout();
        QCOMPARE(changes.last().at(0).toUInt(), 100u); // A brief gap holds the target.
        QTest::qWait(static_cast<int>(abr.CAMERA_QOS_FALLBACK_TIMEOUT_MS - abr.CAMERA_QOS_STALE_TIMEOUT_MS));
        abr.onHeartbeatTimeout();
        QCOMPARE(changes.last().at(0).toUInt(), 50u); // Watchdog can reduce without RSSI.
        rf.rssi = -60;
        abr.handleSiyiLinkStat(rf);
        p.pktSentTotal += 100;
        abr.handleQosCameraConnection({p});
        abr.onHeartbeatTimeout();
        // Recovery needs fresh baseline samples and a stable clear interval.
        for (int i = 0; i < 40 && changes.last().at(0).toUInt() <= 50u; ++i) {
            QTest::qWait(250);
            abr.handleSiyiLinkStat(rf);
            p.pktSentTotal += 100;
            abr.handleQosCameraConnection({p});
        }
        QVERIFY(changes.last().at(0).toUInt() > 50u);
        abr.stop();
    }
};
QTEST_GUILESS_MAIN(RfFallbackTest)
#include "tst_srt_rf_fallback.moc"
