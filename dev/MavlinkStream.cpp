#include "MavlinkStream.h"
#include "MavlinkCrcTable.h"
#include <QtEndian>
#include <algorithm>

static quint16 accumulate(quint16 crc, quint8 byte)
{
    quint8 tmp = byte ^ (crc & 0xff);
    tmp ^= quint8(tmp << 4);
    return quint16((crc >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4));
}

QVector<MavlinkFrameInfo> MavlinkStream::feed(const QByteArray &bytes)
{
    m_buffer.append(bytes);
    QVector<MavlinkFrameInfo> frames;
    int cursor = 0;
    while (cursor < m_buffer.size()) {
        const auto *p = reinterpret_cast<const uchar *>(m_buffer.constData() + cursor);
        const int remaining = int(m_buffer.size()) - cursor;
        if (p[0] != 0xfd && p[0] != 0xfe) { ++cursor; continue; }
        const bool v2 = p[0] == 0xfd;
        const int header = v2 ? 10 : 6;
        if (remaining < header) break;
        if (v2 && (p[2] & ~1)) { ++cursor; continue; }
        const int crcAt = header + p[1];
        const int total = crcAt + 2 + ((v2 && (p[2] & 1)) ? 13 : 0);
        if (remaining < total) break;
        const quint32 id = v2 ? quint32(p[7] | (p[8] << 8) | (p[9] << 16)) : p[5];
        const auto definition = std::lower_bound(std::begin(MavlinkCrcTable), std::end(MavlinkCrcTable), id,
            [](const MavlinkCrcEntry &entry, quint32 value) { return entry.id < value; });
        if (definition == std::end(MavlinkCrcTable) || definition->id != id || p[1] > definition->maxLength) {
            ++cursor;
            continue;
        }
        quint16 crc = 0xffff;
        for (int i = 1; i < crcAt; ++i) crc = accumulate(crc, p[i]);
        crc = accumulate(crc, definition->extra);
        if (crc != qFromLittleEndian<quint16>(p + crcAt)) {
            ++m_crcRejected;
            ++cursor;
            continue;
        }
        frames.append({id, p[v2 ? 5 : 3], p[v2 ? 6 : 4]});
        ++m_framesValid;
        cursor += total;
    }
    m_buffer.remove(0, cursor);
    return frames;
}
