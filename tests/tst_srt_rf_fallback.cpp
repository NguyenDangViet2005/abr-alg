#include <QtTest>
#include "../SRTAdaptiveBitrateStreaming.h"
class RfFallbackTest : public QObject {
    Q_OBJECT
private slots:
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
