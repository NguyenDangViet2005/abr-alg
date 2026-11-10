#include "XBQoSService.h"
#include <QDebug>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QTimer>
#include "ABRFactory.h"
#include "dev/NetworkHandler.h"
#include "dev/RouterNetworkHandler.h"
#include "dev/CameraControl.h"
#include "dev/QoSDebugSender.h"
#include "dev/SiyiLinkHandler.h"
#include "ABRConfigs.h"

XBQoSService::XBQoSService(QObject *parent)
    : QObject(parent)
    , m_abrFactory(nullptr)
    , m_networkHandler(nullptr)
    , m_routerNetworkHandler(nullptr)
    , m_siyiHandler(nullptr)
    , m_cameraControl(nullptr)
    , m_qosDebug(nullptr)
    , m_currentBitrate(0)
    , m_cameraNeedsSync(false)
    , m_cameraKeepAliveTimer(nullptr)
{
    m_cameraControl = new CameraControl(this);
}

XBQoSService::~XBQoSService()
{
    stop();
}

void XBQoSService::setRouterAbrInterfaces(const QStringList &ifaces, bool isC2Configured)
{
    m_routerAbrIfaces = ifaces;
    m_isC2Configured = isC2Configured;
}

void XBQoSService::setProtocolMode(ProtocolMode mode)
{
    m_protocolMode = mode;
}

void XBQoSService::setQosDebug(bool enabled, const QString &host, quint16 port,
                               const QString &logDir)
{
    m_qosDebugEnabled = enabled;
    m_qosDebugHost = host;
    m_qosDebugPort = port;
    m_qosDebugLogDir = logDir;
}

void XBQoSService::setAbrHeadroomPct(int pct)
{
    m_abrHeadroomPct = pct;
}

void XBQoSService::setAbrDisabled(bool disabled)
{
    m_abrDisabled = disabled;
}

void XBQoSService::printStartupBanner()
{
    qInfo() << "============================================================";
    qInfo().noquote() << QString("  XB-QOS-SERVICE : %1 ADAPTIVE BITRATE")
                            .arg(m_protocolMode == ProtocolMode::Srt ? "SRT" : "TCP/ROUTER");
    qInfo() << "============================================================";
    // Loud on purpose: a service left in this mode does not adapt, and the
    // symptom (flat camera bitrate) looks like a bug unless the log says so.
    if (m_abrDisabled) {
        qWarning().noquote() << "[QoS Engine] --disable-abr: camera bitrate is FROZEN. "
                                "ABR still runs, logs and publishes (shadow mode). "
                                "Socket restarts on loss/cliff stay enabled.";
    }
}

void XBQoSService::setupConnections()
{
    m_cameraKeepAliveTimer = new QTimer(this);
    m_cameraKeepAliveTimer->setInterval(5000);
    connect(m_cameraKeepAliveTimer, &QTimer::timeout, this, [this]() {
        // The timer's own running state IS the traffic gate: handleBitrateChange
        // starts it on a live bitrate and stops it on PAUSE. It used to also
        // read srtAbr()->isTrafficActive(), but the SRT engine is inert
        // (m_isTrafficIdle defaults true and only a 12345 QoS packet clears it),
        // so that read was permanently false and silently disabled the
        // keep-alive — the router now owns the camera, so drop it.
        if (m_currentBitrate > VIDEO_DISABLED_BITRATE_KBPS) {
            dispatchToCameraServer(static_cast<int>(m_currentBitrate));
        }
    });

    auto handleBitrateChange = [this](unsigned int newBitrate) {
        m_requestedBitrate = newBitrate;
        newBitrate = static_cast<unsigned int>(m_c2Guard.clamp(static_cast<int>(newBitrate)));
        // Missing RSSI may freeze recovery, but must never block a C2 reduction.
        if (m_protocolMode == ProtocolMode::Srt && newBitrate > m_currentBitrate &&
            m_currentBitrate > 0 &&
            (m_lastValidSiyiMs < 0 || m_c2Clock.elapsed() - m_lastValidSiyiMs > SIYI_LINK_TIMEOUT_MS)) {
            newBitrate = m_currentBitrate;
        }
        // --disable-abr: THE mute. Returning here is the whole feature — this
        // lambda is the only writer of m_currentBitrate and the only caller of
        // dispatchToCameraServer for bitrate, so one early return stops the
        // camera adapting. The keep-alive and the QoS-resync POST then go inert
        // on their own, because both are guarded on m_currentBitrate > 0 and it
        // now stays 0. Deliberately NOT gated downstream: keeping the exit door
        // (dispatchToCameraServer) dumb makes it obvious the mute is upstream.
        // The loss-driven socket restart is NOT muted — see its lambda below,
        // which reads the factory's cached last effective bitrate instead.
        if (m_abrDisabled) {
            return;
        }
        if (newBitrate == m_currentBitrate && !m_cameraNeedsSync) {
            return;
        }
        m_currentBitrate = newBitrate;

        if (newBitrate == VIDEO_DISABLED_BITRATE_KBPS) {
            dispatchToCameraServer(VIDEO_DISABLED_BITRATE_KBPS);
            if (m_cameraKeepAliveTimer) {
                m_cameraKeepAliveTimer->stop();
            }
            return;
        }

        m_resolutionAdapter.updateBitrate(newBitrate);
        dispatchToCameraServer(static_cast<int>(newBitrate));

        if (m_cameraKeepAliveTimer) {
            m_cameraKeepAliveTimer->start();
        }
    };

    // ── Who owns the camera ──────────────────────────────────────────────
    // The per-interface ROUTER ABR drives the camera; the SRT engine is inert
    // (its 12345 path is unused) and must not overwrite the router's decision.
    // A flag, not a deletion: re-enabling SRT is flipping this one line, and the
    // SRT object stays alive as an observer (its signals still fire and log).
    const bool routerAbrOwnsCamera = (m_protocolMode == ProtocolMode::Tcp);

    if (m_abrFactory && m_abrFactory->srtAbr()) {
        if (!routerAbrOwnsCamera) {
            connect(m_abrFactory->srtAbr(), &IAdaptiveBitrateStreaming::bitrateChanged, this, handleBitrateChange);
        } else {
            qInfo().noquote() << "[QoS Engine] ROUTER_ABR_OWNS_CAMERA: SRT engine's "
                                 "bitrateChanged is NOT wired to the camera.";
        }
        connect(m_abrFactory->srtAbr(), &IAdaptiveBitrateStreaming::trafficActiveChanged, this, [this](bool isActive) {
            if (isActive) {
                if (m_currentBitrate > VIDEO_DISABLED_BITRATE_KBPS) {
                    qInfo().noquote() << QString("[QoS Engine] Video traffic resumed! Resyncing camera to %1 kbps").arg(m_currentBitrate);
                    dispatchToCameraServer(static_cast<int>(m_currentBitrate));
                }
            } else {
                qInfo().noquote() << "[QoS Engine] Video traffic idle / Camera off, pausing keep-alive updates.";
            }
        });
        connect(m_abrFactory->srtAbr(), &IAdaptiveBitrateStreaming::videoStreamEnableChanged, this, [](bool isEnabled) {
            if (!isEnabled) {
                qWarning().noquote() << "[C2 Priority] STRICT_CUTOFF: video disabled";
            } else {
                qInfo().noquote() << "[C2 Normal] Video re-enabled";
            }
        });
        connect(m_abrFactory->srtAbr(), &IAdaptiveBitrateStreaming::c2PriorityChanged, this, [](int level, const QString &name) {
            Q_UNUSED(level);
            if (name.contains("Critical") || name.contains("High")) {
                qWarning().noquote() << QString("[C2 Priority] %1").arg(name);
            }
        });
        // Phase 12 (H1 fix): requestStreamRefresh is now a HARD socket restart, so
        // this SRT keyframe ask must respect the SAME ROUTER_ABR_OWNS_CAMERA gate as
        // bitrateChanged above. The SRT engine emits requestKeyframe on congestion
        // transitions with cooldowns of only 800ms–4s and NO 30s token — if it ever
        // fires, an un-gated connect here would restart the video socket every few
        // seconds = permanent video outage, bypassing the router ABR's rate limit.
        // SRT is not implemented today (its 12345 path is inert), so this is a
        // latent-bug guard, not a live fix.
        connect(m_abrFactory->srtAbr(), &IAdaptiveBitrateStreaming::requestKeyframe, this, [this, routerAbrOwnsCamera]() {
            if (routerAbrOwnsCamera) {
                qInfo().noquote() << "[QoS Engine] ROUTER_ABR_OWNS_CAMERA: SRT keyframe ask "
                                     "ignored (would restart the video socket).";
                return;
            }
            if (m_cameraControl && m_currentBitrate > VIDEO_DISABLED_BITRATE_KBPS) {
                m_cameraControl->requestStreamRefresh(static_cast<int>(m_currentBitrate));
            }
        });
    }

    // C2 protection runs at the final output and can reduce video without RSSI.
    m_c2Clock.start();
    auto publishC2 = [this, handleBitrateChange](bool sample) {
        handleBitrateChange(m_requestedBitrate);
        const QString detail = QString("source=SRT_C2 state=%1 rtt_ms=%2 retrans_ps=%3 drop_ps=%4 cap_kbps=%5 requested_kbps=%6 commanded_kbps=%7 shadow=%8 camera_sync_pending=%9")
            .arg(m_c2Guard.name()).arg(m_c2Guard.rtt, 0, 'f', 2)
            .arg(m_c2Guard.retransRate, 0, 'f', 2).arg(m_c2Guard.dropRate, 0, 'f', 2)
            .arg(m_c2Guard.ceiling).arg(m_requestedBitrate).arg(m_currentBitrate).arg(m_abrDisabled ? 1 : 0)
            .arg(m_cameraNeedsSync ? 1 : 0);
        if (sample || m_c2Guard.state == C2BitrateGuard::State::Stale) {
            if (qEnvironmentVariableIntValue("XB_QOS_MONITOR_TELEMETRY") == 1) {
                qInfo().noquote() << "[XB_MONITOR] [C2 Guard]" << detail;
            }
            if (m_qosDebug) m_qosDebug->handleEvent(QStringLiteral("SRT_C2"), QS_EVT_C2_STATE_CHANGE, detail);
        }
    };
    if (m_networkHandler) {
        connect(m_networkHandler, &NetworkHandler::onC2DataReceived, this,
                [this, publishC2](const QVector<SRTPeerStat> &peers) {
            std::vector<C2BitrateGuard::Peer> samples;
            for (const auto &p : peers) {
                samples.push_back({(p.peerAddress + ':' + QString::number(p.peerPort)).toStdString(),
                                   p.msRTT, p.pktRetransTotal, p.pktSndDropTotal, p.isC2Only});
            }
            m_c2Guard.receive(samples, m_c2Clock.elapsed(), static_cast<int>(m_currentBitrate));
            publishC2(true);
        });
    }
    m_c2GuardTimer = new QTimer(this);
    m_c2GuardTimer->setInterval(1000);
    connect(m_c2GuardTimer, &QTimer::timeout, this, [this, publishC2]() {
        const auto before = m_c2Guard.state;
        m_c2Guard.tick(m_c2Clock.elapsed());
        if (before != m_c2Guard.state) publishC2(false);
    });
    m_c2GuardTimer->start();

    if (m_abrFactory) {
        // ── Router ABR → camera ──────────────────────────────────────────
        // The per-interface router ABR's EFFECTIVE (primary) proposal is the
        // one thing that drives the camera. It replaces the SRT bitrateChanged
        // connect above (guarded off by ROUTER_ABR_OWNS_CAMERA). Carries only
        // the primary's number — a spare interface's proposal never lands here.
        if (routerAbrOwnsCamera) {
            connect(m_abrFactory, &ABRFactory::routerAbrEffectiveBitrate, this, handleBitrateChange);
        }

        // ── Router ABR stream-reset → camera video-socket restart (Phase 11/12) ──
        // Two emit sites reach here, both rate-limited by the SHARED 30s token at
        // the instance (ABR_RESET_RATE_LIMIT_MS): the loss ladder's reset band
        // (> ABR_LOSS_RESET_PCT) and the Phase-12 score cliff. requestStreamRefresh
        // is now a HARD socket restart (POST /connections/video/restart), NOT an
        // adapt-bitrate re-apply — the ladder owns the bitrate. The instance owns no
        // socket, so the executor lives here — mirroring the SRT requestKeyframe →
        // requestStreamRefresh connect above. Lambda stays dumb (already rate-limited
        // at the emit site). Guarded on a live bitrate: no restart while the camera
        // is disabled (PAUSE).
        //
        // --disable-abr: the mute in handleBitrateChange leaves m_currentBitrate at
        // 0, so this guard would go permanently false — a camera collapsing under
        // --disable-abr could never be rescued. Read the factory's cached last
        // effective bitrate instead whenever the mute is on: the ABR still computes
        // proposals (shadow mode), so it still knows what the camera is running.
        // requestStreamRefresh() changes NO bitrate — it only drops the socket — so
        // firing it here does not breach the "do not change the bitrate" contract.
        connect(m_abrFactory, &ABRFactory::routerAbrResetRequested, this,
                [this](const QString &iface, const QString &reason) {
            const int cameraKbps = m_abrDisabled
                ? m_abrFactory->lastEffectiveBitrate()
                : static_cast<int>(m_currentBitrate);
            if (m_cameraControl && cameraKbps > static_cast<int>(VIDEO_DISABLED_BITRATE_KBPS)) {
                qWarning().noquote() << QString("[QoS Engine] ABR reset requested (iface=%1, %2) "
                                                "— restarting camera video socket")
                                            .arg(iface).arg(reason);
                m_cameraControl->requestStreamRefresh(cameraKbps);
                // Phase 12: mark the restart moment on the dashboard. Tapped HERE (next
                // to the actual restart, camera-enabled guard passed) so the event means
                // "restart executed", not merely "requested". The dashboard already
                // decodes QS_EVT_SOCKET_RESET_REQUEST (=6) into its event log; detail =
                // the ABR reason ("score-cliff" | "loss>reset_pct").
                if (m_qosDebug) {
                    m_qosDebug->handleEvent(iface, QS_EVT_SOCKET_RESET_REQUEST, reason);
                }
            }
        });

        if (m_networkHandler) {
            connect(m_networkHandler, &NetworkHandler::onQosDataReceived, m_abrFactory, &ABRFactory::onSrtCameraConnection);
            connect(m_networkHandler, &NetworkHandler::onC2DataReceived, m_abrFactory, &ABRFactory::handleC2Data);
        }
        // Router telemetry (UDP 12346). Routed to the per-interface ABR
        // instances (ABRFactory::handleRouterRow → XBAdaptiveBitrateStreaming).
        // Deliberately SEPARATE from the 12345 SRT QoS path below: they
        // describe different things (router's own per-interface view vs.
        // XBSRTFactory's measurement of the SRT stream) and must not be merged.
        if (m_routerNetworkHandler) {
            connect(m_routerNetworkHandler, &RouterNetworkHandler::onRouterRow,
                    m_abrFactory, &ABRFactory::handleRouterRow);
            connect(m_routerNetworkHandler, &RouterNetworkHandler::onRouterTick,
                    m_abrFactory, &ABRFactory::handleRouterTick);
        // Radio strength (row 603) → the owning instance's NetworkReference.
            connect(m_routerNetworkHandler, &RouterNetworkHandler::onNetworkStrength,
                    m_abrFactory, &ABRFactory::handleNetworkStrength);
        // GPS fix (row 604, GLOBAL) → BROADCAST to every instance's NetworkReference.
            connect(m_routerNetworkHandler, &RouterNetworkHandler::onGpsFix,
                    m_abrFactory, &ABRFactory::handleGpsFix);

        // QoS debug channel taps the SAME signals. It only reads, so the tap
        // order relative to the factory does not matter.
            if (m_qosDebug) {
            connect(m_routerNetworkHandler, &RouterNetworkHandler::onRouterRow,
                    m_qosDebug, &QoSDebugSender::handleRouterRow);
            connect(m_routerNetworkHandler, &RouterNetworkHandler::onRouterTick,
                    m_qosDebug, &QoSDebugSender::handleTick);
            // The ABR decision is computed by the factory, so the channel taps
            // the factory's relay rather than each per-interface instance. This
            // is what fills abrBranch/abrReason/maxAbr in every SAMPLE.
            connect(m_abrFactory, &ABRFactory::routerAbrDecision,
                    m_qosDebug, &QoSDebugSender::handleAbrDecision);
            // The ceiling EDGE, so QS_EVT_ABR_CEILING fires the moment it is
            // learned, not on the next 1 Hz SAMPLE.
            connect(m_abrFactory, &ABRFactory::routerAbrCeiling,
                    m_qosDebug, &QoSDebugSender::handleAbrCeiling);
            }
        }
        if (m_siyiHandler) {
            connect(m_siyiHandler, &SiyiLinkHandler::onSiyiLinkUpdated, this, [this](const SiyiLinkStat &stat) {
                if (stat.isAlive && stat.signal > 0 && stat.signal <= 100 && stat.rssi < 0 && stat.rssi >= -120)
                    m_lastValidSiyiMs = m_c2Clock.elapsed();
                else m_lastValidSiyiMs = -1;
            });
            connect(m_siyiHandler, &SiyiLinkHandler::onSiyiLinkLost, this, [this]() { m_lastValidSiyiMs = -1; });
            connect(m_siyiHandler, &SiyiLinkHandler::onSiyiLinkUpdated, m_abrFactory, &ABRFactory::handleSiyiData);
            connect(m_siyiHandler, &SiyiLinkHandler::onSiyiLinkLost, m_abrFactory, &ABRFactory::handleSiyiLost);
        }
        if (m_networkHandler) connect(m_networkHandler, &NetworkHandler::onQosDataReceived, this, [this]() {
            // Khi nhận lại được gói QoS camera (prefix 9990) mà trước đó lệnh gửi camera bị lỗi
            if (m_cameraNeedsSync && m_currentBitrate > VIDEO_DISABLED_BITRATE_KBPS) {
                m_cameraNeedsSync = false;
                qInfo().noquote() << QString("[QoS Engine] Camera stream active, resyncing bitrate to %1 kbps").arg(m_currentBitrate);
                dispatchToCameraServer(static_cast<int>(m_currentBitrate));
            }
        });
    }

    if (m_cameraControl && m_abrFactory) {
        connect(m_cameraControl, &CameraControl::bitrateReported, this, [this](int requestedKbps, int achievedKbps) {
            m_cameraNeedsSync = false;
            if (achievedKbps > 0) {
                if (qAbs(achievedKbps - requestedKbps) > 250) {
                    qWarning().noquote() << QString("[CameraControl] Note: Camera reported bitrate %1 kbps (differs from target %2 kbps)")
                                                .arg(achievedKbps).arg(requestedKbps);
                }
                if (m_protocolMode == ProtocolMode::Srt && m_abrFactory->srtAbr()) {
                    m_abrFactory->srtAbr()->handleCameraReportedBitrate(achievedKbps);
                } else {
                    // TCP/router mode closes the loop through the primary
                    // interface instance that currently owns the camera.
                    m_abrFactory->handleCameraReportedBitrate(achievedKbps);
                }
            }
        });
        connect(m_cameraControl, &CameraControl::bitrateRejected, this, [this](int requestedKbps, const QString &reason) {
            qWarning().noquote() << QString(">>> [QoS] ⚠️ Camera REJECTED command %1 kbps: %2 — ABR will resync upon connection <<<")
                                        .arg(requestedKbps)
                                        .arg(reason);
            m_cameraNeedsSync = true;
        });
    }
}

void XBQoSService::dispatchToCameraServer(int bitrate)
{
    if (m_cameraControl) {
        m_cameraControl->sendAdaptBitrate(bitrate);
    }
}

void XBQoSService::start()
{
    m_c2Guard = C2BitrateGuard(ABR_DEFAULT_MAX_ABR_BITRATE, 250);
    m_requestedBitrate = 0;
    m_lastValidSiyiMs = -1;
    printStartupBanner();

    m_abrFactory = ABRFactory::instance();
    m_abrFactory->init();
    // Phase 09 (headroom): hand the configured percent to the factory BEFORE the
    // router ABR instances are created (startRouterAbr), so each is built with
    // the right cap. Router ABR only; the SRT engine is untouched.
    m_abrFactory->setHeadroomPct(m_abrHeadroomPct);
    if (m_protocolMode == ProtocolMode::Srt) {
        m_abrFactory->startCameraSocketAbr();
    }

    // Per-interface router ABR. Declared list comes from main.cpp (see
    // setRouterAbrInterfaces) — the router's config.ini `interfaces=` is a
    // SEPARATE list and is never regenerated; a mismatch surfaces as the
    // "unknown iface" warning in ABRFactory::handleRouterRow (report risk #5).
    if (m_protocolMode == ProtocolMode::Tcp && !m_routerAbrIfaces.isEmpty()) {
        m_abrFactory->startRouterAbr(m_routerAbrIfaces, m_isC2Configured);
    } else if (m_protocolMode == ProtocolMode::Tcp) {
        qWarning().noquote() << "[QoS Engine] No router ABR interfaces declared — "
                                "per-interface ABR disabled (router rows will be logged only).";
    }

    // Listen for SRT C2 in either mode; router proposals share its final cap.
    m_networkHandler = new NetworkHandler(this);
    if (m_protocolMode == ProtocolMode::Srt) {
        m_siyiHandler = new SiyiLinkHandler(this);
    } else {
        m_routerNetworkHandler = new RouterNetworkHandler(this);
    }

    // Created BEFORE setupConnections so the tap connects to a live object.
    // The debug channel is harmless when disabled: it logs but never opens a
    // socket or file.
    m_qosDebug = new QoSDebugSender(this);
    m_qosDebug->setEnabled(m_qosDebugEnabled);

    setupConnections();

    if (m_networkHandler) m_networkHandler->start(SRT_ABR_QOS_UDP_PORT);
    if (m_siyiHandler) m_siyiHandler->start();

    // RouterNetworkHandler logs its own bind result (success or errorString),
    // so no service-level "listening" line here to avoid claiming success on a
    // failed bind.
    if (m_routerNetworkHandler) m_routerNetworkHandler->start(ROUTER_TELEMETRY_UDP_PORT);

    // Activate the debug channel's I/O (if enabled) and announce the topology.
    // No local file log: capture-recorder on XBLINK stores the stream and
    // forwards it, so a second copy here only duplicates bytes.
    if (m_qosDebugEnabled) {
        m_qosDebug->startUdp(m_qosDebugHost, m_qosDebugPort);
        QVector<QPair<QString, quint8>> topo;
        topo.reserve(m_routerAbrIfaces.size());
        for (const QString &name : m_routerAbrIfaces) {
            topo.append({ name, 1 });   // priority filled by the router's own rows
        }
        m_qosDebug->setTopology(topo, QString(), m_qosDebugPort);
        m_qosDebug->emitConfig();
    } else {
        qInfo().noquote() << "[QoS Engine] QoS debug channel disabled "
                             "(qDebug only, no UDP/file). Enable with --qos-debug.";
    }
    qInfo().noquote() << QString("[QoS Engine] Service started in %1 mode on UDP %2")
                            .arg(m_protocolMode == ProtocolMode::Srt ? "SRT" : "TCP")
                            .arg(m_protocolMode == ProtocolMode::Srt
                                     ? SRT_ABR_QOS_UDP_PORT : ROUTER_TELEMETRY_UDP_PORT);
}

void XBQoSService::stop()
{
    if (m_c2GuardTimer) m_c2GuardTimer->stop();
    if (m_cameraKeepAliveTimer) {
        m_cameraKeepAliveTimer->stop();
    }
    if (m_networkHandler) {
        m_networkHandler->stop();
    }
    if (m_routerNetworkHandler) {
        m_routerNetworkHandler->stop();
    }
    if (m_siyiHandler) {
        m_siyiHandler->stop();
    }
    if (m_qosDebug) {
        m_qosDebug->stopFileLog();
        m_qosDebug->stopUdp();
    }
    if (m_abrFactory) {
        m_abrFactory->stopRouterAbr();
        m_abrFactory->stopCameraSocketAbr();
    }
}
