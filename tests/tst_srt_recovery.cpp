#include <QtTest>
#include "../SRTAdaptiveBitrateStreaming.h"
#include "../ABRFactory.h"

class SrtRecoveryTest : public QObject
{
    Q_OBJECT
    static qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }

    static SiyiLinkStat rf()
    {
        SiyiLinkStat s;
        s.isAlive = true;
        s.signal = 100;
        s.rssi = -50;
        return s;
    }

    static SRTPeerStat prepare(SRTAdaptiveBitrateStreaming &abr)
    {
        abr.start();
        abr.m_heartbeatTimer->stop();
        abr.handleSiyiLinkStat(rf());
        SRTPeerStat peer;
        peer.peerAddress = "camera";
        peer.peerPort = 1234;
        peer.msRTT = 7;
        peer.mbpsBandwidth = 5;
        peer.mbpsSendRate = 1;
        peer.pktSentTotal = 100;
        abr.handleQosCameraConnection({peer});
        // Seed a completed baseline; exercise real QoS decisions without sleeps.
        abr.m_rttMinSeeded = true;
        abr.m_hasLastSent = true;
        abr.m_lastSentTotal = peer.pktSentTotal;
        abr.m_recovery = SRTRecoveryState();
        ready(abr);
        return peer;
    }

    static void ready(SRTAdaptiveBitrateStreaming &abr)
    {
        abr.m_lastBitrateIncrTime = now() - 2100;
        abr.m_lastBitrateChangeTime = now() - 2100;
        abr.m_clearSinceMs = now() - 10000;
        abr.m_cooldownUntilMs = 0;
    }

    static void sample(SRTAdaptiveBitrateStreaming &abr, SRTPeerStat &peer)
    {
        peer.pktSentTotal += 100;
        abr.handleQosCameraConnection({peer});
    }

private slots:
    void factoryUsesSrtCeiling()
    {
        ABRFactory factory;
        factory.startCameraSocketAbr();
        QVERIFY(factory.srtAbr());
        QCOMPARE(factory.srtAbr()->m_maxBitrateKbps, 6000u);
        QCOMPARE(factory.srtAbr()->m_c2RateTracker.ceiling, 6000);
        factory.stopCameraSocketAbr();
    }

    void srtCeilingIsIndependentAndCanReachSixMbps()
    {
        QCOMPARE(static_cast<unsigned int>(ABR_DEFAULT_MAX_ABR_BITRATE), 4000u);
        QCOMPARE(SRTTuning::DEFAULT_MAX_BITRATE_KBPS, 6000u);
        QCOMPARE(RF_ENVIRONMENTS[0].ceilingKbps, 6000u);
        SRTAdaptiveBitrateStreaming abr;
        auto peer = prepare(abr);
        QCOMPARE(abr.currentBitrate(), 1000u);
        QCOMPARE(abr.m_c2RateTracker.ceiling, 6000);
        abr.m_currentBitrateKbps = 4000;
        abr.m_bwHistory = {8.0};
        peer.mbpsBandwidth = 8.0;
        for (int i = 0; i < 6; ++i) {
            const unsigned int current = abr.currentBitrate();
            peer.mbpsSendRate = current / 1000.0;
            if (abr.m_recovery.awaitingFeedback())
                abr.m_recovery.bitrateChanged(now() - 1000,
                    abr.m_recovery.rollbackTarget(), current);
            ready(abr);
            sample(abr, peer);
            QVERIFY(abr.currentBitrate() <= 6000u);
            if (i == 0) QVERIFY(abr.currentBitrate() > 4000u);
        }
        QCOMPARE(abr.currentBitrate(), 6000u);
        abr.setSerialConnected(false);
        QCOMPARE(abr.m_c2RateTracker.ceiling, 6000);
        abr.reset(0);
        QCOMPARE(abr.currentBitrate(), 1000u);
    }

    void recoveryStepTapersAndClamps()
    {
        QCOMPARE(SRTRecoveryState::step(1000, 3000), 800u);
        QCOMPARE(SRTRecoveryState::step(1800, 3000), 600u);
        QCOMPARE(SRTRecoveryState::step(2400, 3000), 300u);
        QCOMPARE(SRTRecoveryState::step(2700, 3000), 200u);
        QCOMPARE(SRTRecoveryState::step(2900, 3000), 100u);
        QCOMPARE(SRTRecoveryState::step(3000, 3000), 0u);
        QCOMPARE(SRTRecoveryState::step(3000, 1000), 0u);
    }

    void memoryNeedsSustainedActualTrafficAndExpires()
    {
        SRTRecoveryState memory;
        memory.observe(1000, 3000, 1000, true);
        memory.observe(5000, 3000, 1000, true);
        QCOMPARE(memory.provenTarget(5000), 0u);
        memory.observe(6000, 3000, 3000, true);
        memory.observe(7000, 3000, 3000, false);
        memory.observe(8000, 3000, 3000, true);
        memory.observe(9000, 3000, 3000, true);
        QCOMPARE(memory.provenTarget(9000), 0u);
        memory.observe(9500, 3000, 3000, true);
        QCOMPARE(memory.provenTarget(9500), 3000u);
        memory.bitrateChanged(10000, 3000, 1000);
        memory.observe(11000, 1000, 1000, true);
        memory.observe(13000, 1000, 1000, true);
        QCOMPARE(memory.provenTarget(13000), 3000u);
        QCOMPARE(memory.provenTarget(39501), 0u);
    }

    void pendingStepNeedsNewFeedbackAtTarget()
    {
        SRTRecoveryState memory;
        memory.bitrateChanged(1000, 1000, 1800);
        memory.observe(1000, 1800, 1800, true);
        QVERIFY(memory.awaitingFeedback());
        memory.observe(2000, 1800, 1000, true);
        QVERIFY(memory.awaitingFeedback());
        QCOMPARE(memory.rollbackTarget(), 1000u);
        memory.observe(3000, 1800, 1800, false);
        QVERIFY(memory.awaitingFeedback());
        memory.observe(4000, 1800, 1800, true);
        QVERIFY(!memory.awaitingFeedback());
        memory.bitrateChanged(5000, 1800, 1900);
        memory.observe(6000, 1900, 1800, true);
        QVERIFY(memory.awaitingFeedback()); // Old load cannot confirm a small probe.
        memory.observe(7000, 1900, 1900, true);
        QVERIFY(!memory.awaitingFeedback());
    }

    void fastRampWaitsForActualFreshFeedback()
    {
        SRTAdaptiveBitrateStreaming abr;
        auto peer = prepare(abr);
        sample(abr, peer);
        QCOMPARE(abr.currentBitrate(), 1800u);
        ready(abr);
        abr.handleQosCameraConnection({peer}); // Repeated counters.
        QCOMPARE(abr.currentBitrate(), 1800u);
        sample(abr, peer); // New counters, but encoder still at 1000.
        QCOMPARE(abr.currentBitrate(), 1800u);
        abr.m_recovery.bitrateChanged(now() - 1000, 1000, 1800);
        peer.mbpsSendRate = 1.8;
        sample(abr, peer);
        QCOMPARE(abr.currentBitrate(), 2600u);
    }

    void recoveringWithinSameRfRegionUsesProvenRate()
    {
        SRTAdaptiveBitrateStreaming abr;
        auto peer = prepare(abr);
        abr.m_rfRecoveryRampActive = false;
        abr.m_recovery.observe(now() - 3000, 3000, 3000, true);
        abr.m_recovery.observe(now() - 1000, 3000, 3000, true);
        sample(abr, peer);
        QCOMPARE(abr.currentBitrate(), 1800u);
    }

    void recoveryHonorsLearnedAndBandwidthCaps()
    {
        SRTAdaptiveBitrateStreaming abr;
        auto peer = prepare(abr);
        abr.m_lastCongestedBitrate = 1300;
        sample(abr, peer);
        QCOMPARE(abr.currentBitrate(), 1200u); // Taper toward 1300.
        SRTAdaptiveBitrateStreaming limited;
        auto limitedPeer = prepare(limited);
        limitedPeer.mbpsBandwidth = 1.5;
        sample(limited, limitedPeer);
        QVERIFY(limited.currentBitrate() <= 1350u); // Raw estimate caps recovery.
        SRTAdaptiveBitrateStreaming dipped;
        auto dippedPeer = prepare(dipped);
        dippedPeer.mbpsBandwidth = 1.0; // Raw cap below current; average still high.
        sample(dipped, dippedPeer);
        QCOMPARE(dipped.currentBitrate(), 1000u);
    }

    void rawRttRiseBlocksIncreaseBeforeMedianCatchesUp()
    {
        SRTAdaptiveBitrateStreaming abr;
        auto peer = prepare(abr);
        abr.m_previousRawRecoveryRtt = 7;
        peer.msRTT = 11;
        sample(abr, peer);
        QCOMPARE(abr.currentBitrate(), 1000u);
    }

    void unconfirmedIncreaseRollsBackOnLoss()
    {
        SRTAdaptiveBitrateStreaming abr;
        auto peer = prepare(abr);
        sample(abr, peer);
        QCOMPARE(abr.currentBitrate(), 1800u);
        ++peer.pktSndLossTotal;
        sample(abr, peer);
        QVERIFY(abr.currentBitrate() <= 1000u);
        QVERIFY(!abr.m_rfRecoveryRampActive);
    }

    void degradedC2DisablesFastRecovery()
    {
        SRTAdaptiveBitrateStreaming abr;
        auto peer = prepare(abr);
        abr.m_c2Priority = SRTAdaptiveBitrateStreaming::C2PriorityLevel::High;
        sample(abr, peer);
        QVERIFY(abr.currentBitrate() <= 1200u);
    }

    void recoveryAndProbeRespectC2Ceiling()
    {
        SRTAdaptiveBitrateStreaming abr;
        auto peer = prepare(abr);
        abr.m_c2RateTracker.ceiling = 1300;
        sample(abr, peer);
        QCOMPARE(abr.currentBitrate(), 1200u);
        SRTAdaptiveBitrateStreaming probing;
        auto probePeer = prepare(probing);
        probing.m_c2RateTracker.ceiling = 1000;
        probing.m_lastCongestedBitrate = 1000;
        probing.m_probeHealthySince = now() - 6000;
        probing.m_lastProbeTime = now() - 6000;
        sample(probing, probePeer);
        QCOMPARE(probing.currentBitrate(), 1000u);
        QCOMPARE(probing.m_probePreviousCeiling, 0u);
    }

    void probeLiftsOnlyOneStepThenRollsBackAndBacksOff()
    {
        SRTAdaptiveBitrateStreaming abr;
        auto peer = prepare(abr);
        abr.m_lastCongestedBitrate = 1000;
        abr.m_probeHealthySince = now() - 6000;
        abr.m_lastProbeTime = now() - 6000;
        sample(abr, peer);
        QCOMPARE(abr.currentBitrate(), 1100u);
        QCOMPARE(abr.m_probePreviousCeiling, 1000u);
        QCOMPARE(abr.m_lastCongestedBitrate, 1100u);
        ++peer.pktSndLossTotal;
        sample(abr, peer);
        QVERIFY(abr.currentBitrate() <= 1000u);
        QCOMPARE(abr.m_lastCongestedBitrate, 1000u);
        QCOMPARE(abr.m_probeRetryDelayMs, 10000);
        QCOMPARE(abr.m_probePreviousCeiling, 0u);
    }

    void probeConfirmsOnlyAfterCarryingNewRate()
    {
        SRTAdaptiveBitrateStreaming abr;
        auto peer = prepare(abr);
        abr.m_lastCongestedBitrate = 1000;
        abr.m_probeHealthySince = now() - 6000;
        abr.m_lastProbeTime = now() - 6000;
        sample(abr, peer);
        abr.m_recovery.bitrateChanged(now() - 3000, 1000, 1100);
        abr.m_probeStartedAt = now() - 3000;
        ready(abr);
        peer.mbpsSendRate = 0.5;
        sample(abr, peer);
        QCOMPARE(abr.m_probePreviousCeiling, 1000u);
        peer.mbpsSendRate = 1.1;
        sample(abr, peer);
        QCOMPARE(abr.m_probePreviousCeiling, 1000u);
        // Only time spent actually carrying the probe counts as confirmation.
        abr.m_probeStartedAt = now() - 2100;
        sample(abr, peer);
        QCOMPARE(abr.m_probePreviousCeiling, 0u);
        QCOMPARE(abr.m_lastCongestedBitrate, 1100u);
    }

    void resetAndPeerChangeInvalidateRecoveryMemory()
    {
        SRTAdaptiveBitrateStreaming abr;
        auto peer = prepare(abr);
        abr.m_recovery.observe(now() - 3000, 3000, 3000, true);
        abr.m_recovery.observe(now() - 1000, 3000, 3000, true);
        peer.peerAddress = "new-camera";
        sample(abr, peer);
        QCOMPARE(abr.m_recovery.provenTarget(now()), 0u);
        QVERIFY(!abr.m_rfRecoveryRampActive);
        abr.reset(1000);
        QCOMPARE(abr.m_recovery.provenTarget(now()), 0u);
        QVERIFY(!abr.m_recovery.awaitingFeedback());
    }

    void counterResetInvalidatesMemoryAndBlocksIncrease()
    {
        SRTAdaptiveBitrateStreaming abr;
        auto peer = prepare(abr);
        abr.m_recovery.observe(now() - 3000, 3000, 3000, true);
        abr.m_recovery.observe(now() - 1000, 3000, 3000, true);
        peer.pktSentTotal = 1;
        abr.handleQosCameraConnection({peer});
        QCOMPARE(abr.m_recovery.provenTarget(now()), 0u);
        QCOMPARE(abr.currentBitrate(), 1000u);
        QVERIFY(!abr.m_rfRecoveryRampActive);
    }

    void recoveryRequiresDecisionInterval()
    {
        SRTAdaptiveBitrateStreaming abr;
        auto peer = prepare(abr);
        abr.m_lastBitrateIncrTime = now() - 500;
        sample(abr, peer);
        QCOMPARE(abr.currentBitrate(), 1000u);
        abr.m_lastBitrateIncrTime = now() - 1100;
        sample(abr, peer);
        QCOMPARE(abr.currentBitrate(), 1800u);
        SRTAdaptiveBitrateStreaming nearCap;
        auto nearPeer = prepare(nearCap);
        nearCap.m_lastCongestedBitrate = 1300;
        nearCap.m_lastBitrateIncrTime = now() - 1100;
        sample(nearCap, nearPeer);
        QCOMPARE(nearCap.currentBitrate(), 1000u); // Near target keeps 2 s cadence.
    }

    void probeRequiresContinuousHealthAndHonorsBackoff()
    {
        SRTAdaptiveBitrateStreaming abr;
        auto peer = prepare(abr);
        abr.m_lastCongestedBitrate = 1000;
        abr.m_probeHealthySince = now() - 6000;
        abr.m_lastProbeTime = now() - 6000;
        abr.m_probeRetryDelayMs = 10000;
        sample(abr, peer);
        QCOMPARE(abr.currentBitrate(), 1000u);
        abr.m_lastProbeTime = now() - 11000;
        abr.m_previousRawRecoveryRtt = 7;
        peer.msRTT = 11;
        sample(abr, peer);
        QCOMPARE(abr.m_probeHealthySince, 0);
        QCOMPARE(abr.currentBitrate(), 1000u);
        peer.msRTT = 7;
        sample(abr, peer);
        QCOMPARE(abr.currentBitrate(), 1000u);
        QCOMPARE(abr.m_probePreviousCeiling, 0u);
    }
};

QTEST_GUILESS_MAIN(SrtRecoveryTest)
#include "tst_srt_recovery.moc"
