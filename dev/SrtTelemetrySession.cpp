#include "SrtTelemetrySession.h"
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <vector>

namespace XbTelemetry {
static constexpr quint32 SeqMask = 0x7fffffff;

void RttTracker::add(qint64 now, double value, qint64 staleMs)
{
    if (!std::isfinite(value) || value <= 0) return;
    if (m_last < 0 || now - m_last > staleMs) {
        *this = RttTracker();
        m_smoothed = value;
    } else {
        const double alpha = 1 - std::exp(-qMax(qint64(0), now - m_last) / 1000.0);
        m_smoothed += alpha * (value - m_smoothed);
    }
    m_last = now;
    m_samples.emplace_back(now, value);
    if (m_samples.size() > 10000) m_samples.pop_front();
}

void RttTracker::snapshot(C2TelemetryStat &out, qint64 now, qint64 staleMs, double elapsed)
{
    while (!m_samples.empty() && now - m_samples.front().first > 60000) m_samples.pop_front();
    out.rttAgeMs = m_last < 0 ? -1 : qMax(qint64(0), now - m_last);
    out.rttValid = out.peerAckValid && m_last >= 0 && out.rttAgeMs <= staleMs && !m_samples.empty();
    if (out.rttValid) {
        out.rttMs = m_samples.back().second;
        out.rttSmoothedMs = m_smoothed;
        out.baselineValid = m_samples.size() >= 5 && m_samples.back().first - m_samples.front().first >= 3000;
        if (out.baselineValid) {
            std::vector<double> ordered;
            for (const auto &sample : m_samples) ordered.push_back(sample.second);
            std::sort(ordered.begin(), ordered.end());
            out.rttBaselineMs = ordered[(ordered.size() - 1) / 10];
            out.rttInflationMs = qMax(0.0, m_smoothed - out.rttBaselineMs);
            out.rttRatio = m_smoothed / out.rttBaselineMs;
        }
        if (m_previousReport >= 0 && m_last > m_previousReport && elapsed > 0) {
            out.rttTrendValid = true;
            out.rttTrendMsPerSecond = (m_smoothed - m_previousSmoothed) / elapsed;
        }
        m_previousReport = m_last;
        m_previousSmoothed = m_smoothed;
    } else {
        m_previousReport = -1;
    }
}

void Session::gap(quint32 count)
{
    counts.gaps += count;
    parser.reset();
    m_parts.clear();
    m_messageNumber.reset();
}

void Session::receive(const QByteArray &payload, qint64 now, quint8 systemId, quint8 componentId, qint64 staleMs)
{
    lastSeen = now;
    const auto *p = reinterpret_cast<const uchar *>(payload.constData());
    const quint32 first = qFromBigEndian<quint32>(p);
    const quint32 flags = qFromBigEndian<quint32>(p + 4);
    if (first & 0x80000000) {
        const quint32 kind = (first >> 16) & 0x7fff;
        if (kind == 3 && payload.size() >= 20) ++counts.nak;
        if (kind == 2 && payload.size() >= 20) {
            lastAck = now;
            if (payload.size() >= 32) rtt.add(now, qFromBigEndian<quint32>(p + 20) / 1000.0, staleMs);
        }
        return;
    }
    ++counts.data;
    if (flags & (1u << 26)) ++counts.retransmits;
    if ((flags >> 27) & 3) {
        ++encryptedPackets;
        ++counts.invalid; // Encrypted payload cannot be decoded.
        gap(0);
        m_expected.reset();
        m_pending.clear();
        return;
    }
    const quint32 sequence = first & SeqMask;
    if (!m_expected) m_expected = sequence;
    const quint32 distance = (sequence - *m_expected) & SeqMask;
    if (distance >= (1u << 30) || m_pending.count(sequence)) return;
    m_pending.emplace(sequence, Pending{now, flags, payload.mid(16)});
    flush(now, systemId, componentId);
}

void Session::flush(qint64 now, quint8 systemId, quint8 componentId)
{
    while (!m_pending.empty()) {
        auto packet = m_pending.find(*m_expected);
        if (packet == m_pending.end()) {
            qint64 oldest = now;
            auto nearest = m_pending.begin();
            for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
                oldest = qMin(oldest, it->second.time);
                if (((it->first - *m_expected) & SeqMask) < ((nearest->first - *m_expected) & SeqMask)) nearest = it;
            }
            if (now - oldest < 250 && m_pending.size() < 256) return;
            gap((nearest->first - *m_expected) & SeqMask);
            m_expected = nearest->first;
            packet = nearest;
        }
        const Pending value = packet->second;
        m_pending.erase(packet);
        m_expected = (*m_expected + 1) & SeqMask;
        const quint32 position = value.flags >> 30;
        const quint32 number = value.flags & 0x03ffffff;
        if (position == 3) {
            m_parts.clear();
            m_messageNumber.reset();
            application(value.body, value.time, systemId, componentId);
        } else if (position == 2) {
            m_parts = value.body;
            m_messageNumber = number;
        } else if (m_messageNumber && *m_messageNumber == number) {
            m_parts += value.body;
            if (m_parts.size() > 65535) {
                ++counts.invalid;
                gap(0);
            } else if (position == 1) {
                application(m_parts, value.time, systemId, componentId);
                m_parts.clear();
                m_messageNumber.reset();
            }
        } else {
            ++counts.invalid;
            parser.reset();
        }
    }
}

void Session::application(const QByteArray &body, qint64 time, quint8 systemId, quint8 componentId)
{
    lastXbPrefix = body.left(16).toHex();
    if (body.startsWith(QByteArray::fromHex("fd0000000030"))) return;
    const int prefix = outgoing ? 6 : 7;
    if (body.size() < prefix || quint8(body[0]) != 0xfd) {
        ++counts.invalid;
        parser.reset();
        return;
    }
    if (body[prefix - 1] != 'c') return;
    ++counts.chunks;
    counts.bytes += quint64(body.size() - prefix);
    const auto frames = parser.feed(body.mid(prefix));
    for (const auto &frame : frames) {
        lastMavlinkSource = QString::number(frame.systemId) + '/' + QString::number(frame.componentId);
        if (frame.systemId != systemId || frame.componentId != componentId) continue;
        ++counts.frames;
        // Reorder buffering must not make old data look newly received.
        lastFc = qMax(lastFc, time);
        if (frame.messageId == 0) lastHeartbeat = qMax(lastHeartbeat, time);
    }
}

}
