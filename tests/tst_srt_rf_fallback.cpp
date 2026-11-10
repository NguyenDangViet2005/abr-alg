#include <QtTest>
#include "../SRTAdaptiveBitrateStreaming.h"
class RfFallbackTest : public QObject {
    Q_OBJECT
private slots:
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
